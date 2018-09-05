#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <wchar.h>
#include <xmms/util.h>
#include "tags.h"

struct APETagFooterStruct {
    unsigned char ID[8];
    unsigned char Version[4];
    unsigned char Length[4];
    unsigned char TagCount[4];
    unsigned char Flags[4];
    unsigned char Reserved[8];
};

typedef struct {
    char *key;
    size_t keylen;
    unsigned char *value;
    size_t valuelen;
    unsigned int flags;
} TagItem;

unsigned long
Read_LE_Uint32(const unsigned char *p)
{
    return ((unsigned long) p[0] << 0) |
        ((unsigned long) p[1] << 8) |
        ((unsigned long) p[2] << 16) | ((unsigned long) p[3] << 24);
}

// Convert UTF-8 coded string to UNICODE
// Return number of characters converted
int
utf8ToUnicode(const char *lpMultiByteStr, wchar_t * lpWideCharStr,
              int cmbChars)
{
    const unsigned char *pmb = (unsigned char *) lpMultiByteStr;
    unsigned short *pwc = (unsigned short *) lpWideCharStr;
    const unsigned char *pmbe;
    size_t cwChars = 0;

    if (cmbChars >= 0) {
        pmbe = pmb + cmbChars;
    }
    else {
        pmbe = NULL;
    }

    while ((pmbe == NULL) || (pmb < pmbe)) {
        char mb = *pmb++;
        unsigned int cc = 0;
        unsigned int wc;

        while ((cc < 7) && (mb & (1 << (7 - cc)))) {
            cc++;
        }

        if (cc == 1 || cc > 6)  // illegal character combination for UTF-8
            continue;

        if (cc == 0) {
            wc = mb;
        }
        else {
            wc = (mb & ((1 << (7 - cc)) - 1)) << ((cc - 1) * 6);
            while (--cc > 0) {
                if (pmb == pmbe)    // reached end of the buffer
                    return cwChars;
                mb = *pmb++;
                if (((mb >> 6) & 0x03) != 2)    // not part of multibyte character
                    return cwChars;
                wc |= (mb & 0x3F) << ((cc - 1) * 6);
            }
        }

        if (wc & 0xFFFF0000)
            wc = L'?';
        *pwc++ = wc;
        cwChars++;
        if (wc == L'\0')
            return cwChars;
    }

    return cwChars;
}

void
tag_insert(char *buffer, const char *value, long unsigned int len,
           long unsigned int maxlen, bool decode_utf8)
{
    char *p;
    wchar_t wValue[MAX_LEN];
    char temp[MAX_LEN];
    long unsigned int c;
    const wchar_t *src = wValue;

    if (len >= maxlen)
        len = maxlen - 1;
    if (decode_utf8) {
        if ((c = utf8ToUnicode(value, wValue, len)) <= 0)
            return;
        if (wValue[c] != L'\0')
            wValue[c++] = L'\0';
        if ((c = wcsrtombs(temp, &src, MAX_LEN, NULL)) == 0)
            return;
    }
    else {
        c = len;
        strncpy(temp, value, len);
        while (temp[len - 1] == 0x20 || len < 1) {
            len--;
        }
        temp[len] = '\0';
    }

    //if ( *buffer == '\0' ) {    // new value
    p = buffer;
    //} else {                    // append to existing value
    //    p = strchr (buffer, '\0' );
    //    p += sprintf ( p, ", " );
    //}

    if ((p - buffer) + c >= maxlen)
        c = maxlen - (p - buffer) - 1;
    strncpy(p, temp, c);
    p[c] = '\0';
}

// Returns the Type of Tag (Ape or ID3)
int
GetTageType(FILE * fp)
{
    struct APETagFooterStruct T;
    unsigned char tagheader[3];
    int size;

    if (fp == NULL) {
        return TAG_NONE;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
        return TAG_NONE;
    size = ftell(fp);
    if (fseek(fp, size - sizeof T, SEEK_SET) != 0)
        return TAG_NONE;
    if (fread(&T, 1, sizeof T, fp) != sizeof T)
        return TAG_NONE;
    if (memcmp(T.ID, "APETAGEX", sizeof T.ID) == 0)
        return TAG_APE;
    if (fseek(fp, -128L, SEEK_END) != 0)
        return TAG_NONE;
    if (fread(tagheader, 1, 3, fp) != 3)
        return TAG_NONE;
    if (0 == memcmp(tagheader, "TAG", 3))
        return TAG_ID3;
    return TAG_NONE;
}


int
ReadID3Tag(FILE * fp, ape_tag * Tag)
{
    char *tag;
    char *buff;
    unsigned int genre;

    buff = (char *) malloc(128);

    *(Tag->title) = '\0';
    *(Tag->artist) = '\0';
    *(Tag->album) = '\0';
    *(Tag->comment) = '\0';
    *(Tag->genre) = '\0';
    *(Tag->track) = '\0';
    *(Tag->year) = '\0';

    if (fseek(fp, -128L, SEEK_END) != 0) {
        free (buff);
        return 0;
    }
    if (fread(buff, 1, 128, fp) != 128) {
        free (buff);
        return 0;
    }
    tag = buff;
    tag_insert(Tag->title, (tag + 3), 30, 32, false);
    tag_insert(Tag->artist, (tag + 33), 30, 32, false);
    tag_insert(Tag->album, (tag + 63), 30, 32, false);
    tag_insert(Tag->year, (tag + 93), 4, 32, false);
    tag_insert(Tag->comment, (tag + 97), 30, 32, false);
    genre = (unsigned char) tag[127];
    if (genre >= sizeof(GenreList) / sizeof(int))
        genre = 12;
    tag_insert(Tag->genre, GenreList[genre], 30, 32, false);
    sprintf(tag, "%d", tag[126]);
    tag_insert(Tag->track, tag, 30, 32, false);
    free(buff);
    return 1;
}

// Reads APE v2.0 tag
int
ReadAPE2Tag(FILE * fp, ape_tag * Tag)
{
    unsigned long vsize;
    unsigned long isize;
    unsigned long flags;
    unsigned char *buff;
    unsigned char *p;
    unsigned char *end;
    struct APETagFooterStruct T;
    unsigned long TagLen;
    unsigned long TagCount;
    long size;

    *(Tag->title) = '\0';
    *(Tag->artist) = '\0';
    *(Tag->album) = '\0';
    *(Tag->comment) = '\0';
    *(Tag->genre) = '\0';
    *(Tag->track) = '\0';
    *(Tag->year) = '\0';

    if (fseek(fp, 0, SEEK_END) != 0)
        return 0;
    size = ftell(fp);
    if (fseek(fp, size - sizeof T, SEEK_SET) != 0)
        return 0;
    if (fread(&T, 1, sizeof T, fp) != sizeof T)
        return 0;
    if (memcmp(T.ID, "APETAGEX", sizeof T.ID) != 0)
        return 0;
    if (Read_LE_Uint32(T.Version) != 2000)
        return 0;
    TagLen = Read_LE_Uint32(T.Length);
    if (TagLen < sizeof T)
        return 0;
    if (fseek(fp, size - TagLen, SEEK_SET) != 0)
        return 0;
    if ((buff = (unsigned char *) malloc(TagLen)) == NULL)
        return 0;
    if (fread(buff, 1, TagLen - sizeof T, fp) != TagLen - sizeof T) {
        free(buff);
        return 0;
    }

    TagCount = Read_LE_Uint32(T.TagCount);
    end = buff + TagLen - sizeof(T);
    for (p = buff; p < end && TagCount--;) {
        vsize = Read_LE_Uint32(p);
        p += 4;
        flags = Read_LE_Uint32(p);
        p += 4;
        isize = strlen((char *) p);

        if (isize > 0 && vsize > 0) {
            if (!(flags & 1 << 1)) {    // insert UTF-8 string (skip binary values)
                if (!strcasecmp((char *) p, "Title")) {
                    tag_insert(Tag->title, (char *) (p + isize + 1), vsize,
                               MAX_LEN, false);
                }
                else if (!strcasecmp((char *) p, "Artist")) {
                    tag_insert(Tag->artist, (char *) (p + isize + 1), vsize,
                               MAX_LEN, false);
                }
                else if (!strcasecmp((char *) p, "Album")) {
                    tag_insert(Tag->album, (char *) (p + isize + 1), vsize,
                               MAX_LEN, false);
                }
                else if (!strcasecmp((char *) p, "Comment")) {
                    tag_insert(Tag->comment, (char *) (p + isize + 1), vsize,
                               MAX_LEN, false);
                }
                else if (!strcasecmp((char *) p, "Genre")) {
                    tag_insert(Tag->genre, (char *) (p + isize + 1), vsize,
                               MAX_LEN, false);
                }
                else if (!strcasecmp((char *) p, "Track")) {
                    tag_insert(Tag->track, (char *) (p + isize + 1), vsize,
                               128, false);
                }
                else if (!strcasecmp((char *) p, "Year")) {
                    tag_insert(Tag->year, (char *) (p + isize + 1), vsize,
                               128, false);
                }
            }
        }
        p += isize + 1 + vsize;
    }
    free(buff);
    return 1;
}

int
DeleteTag(char *filename)
{

    FILE *fp = fopen(filename, "rb+");
    int tagtype;
    int fd;
    long filelength = 0;
    long dellength = -1;
    char *tagheader;
    unsigned long *apelength;
    int res = -1;

    if (fp == NULL) {
        char text[256];

        sprintf(text, "File \"%s\" not found or is read protected!\n",
                filename);
        xmms_show_message("File-Error", (gchar *) text, "Ok", FALSE, NULL,
                          NULL);
        return -1;
    }
    tagtype = GetTageType(fp);

    // get Length of File
    fseek(fp, 0L, SEEK_END);
    filelength = ftell(fp);

    apelength = (unsigned long *) malloc(4);
    tagheader = (char *) malloc(9);

    if (tagtype == TAG_ID3) {
        dellength = 128L;
    }
    else if (tagtype == TAG_APE) {
        fseek(fp, -32L, SEEK_END);
        fread(tagheader, 8, 1, fp);
        if (0 == memcmp(tagheader, "APETAGEX", 8)) {
            fseek(fp, -20L, SEEK_END);
            fread(apelength, 4, 1, fp);
            dellength = *apelength + 32;
        }
    }


    if (dellength > -1)         //if TAG was found, delete it
    {
        fd = open(filename, O_RDWR);
        res = ftruncate(fd, (off_t) (filelength - dellength));
        close(fd);
    }

    free(tagheader);
    free(apelength);

    //returns 0 if everything is ok
    return res;
}

// Returns bytes used in APE-Tag for this value
int
addValue(TagItem * item, char *key, char *value)
{
    item->keylen = strlen(key);
    item->valuelen = strlen(value);
    item->key = (char *) malloc(item->keylen + 1);
    item->value = (unsigned char *) malloc(item->valuelen + 1);
    strcpy((char *) item->value, value);
    strcpy(item->key, key);
    item->flags = 0;
    return (9 + item->keylen + item->valuelen);
}

int
WriteAPE2Tag(char *filename, ape_tag * Tag)
{
    FILE *fp;
    unsigned char H[32] = "APETAGEX";
    unsigned long Version = 2000;
    unsigned char dw[8];
    unsigned long estimatedbytes = 32;  // 32 byte footer + all items, these are the 32 bytes footer, the items are added later
    long writtenbytes = -32;    // actually writtenbytes-32, which should be equal to estimatedbytes (= footer + all items)
    unsigned int TagCount = 0;
    TagItem T[7];


    // Delete Tag if there is one
    fp = fopen(filename, "rb+");
    if (fp == NULL) {
        char text[256];

        sprintf(text, "File \"%s\" not found or is read protected!\n",
                filename);
        xmms_show_message("File-Error", (gchar *) text, "Ok", FALSE, NULL,
                          NULL);
        return -1;
    }

    int tagtype = GetTageType(fp);

    if (tagtype != TAG_NONE)
        if (DeleteTag(filename) != 0)
            return 0;

    // Produce TagItem-Array
    if (strlen(Tag->title) > 0) {
        char *value = (char *) malloc(strlen(Tag->title) + 1);

        strcpy(value, Tag->title);
        int res = addValue(&T[TagCount], "Title", value);

        estimatedbytes += res;
        if (res > 0)
            TagCount++;
        free(value);
    }

    if (strlen(Tag->artist) > 0) {
        char *value = (char *) malloc(strlen(Tag->artist) + 1);

        strcpy(value, Tag->artist);
        int res = addValue(&T[TagCount], "Artist", value);

        estimatedbytes += res;
        if (res > 0)
            TagCount++;
        free(value);
    }

    if (strlen(Tag->album) > 0) {
        char *value = (char *) malloc(strlen(Tag->album) + 1);

        strcpy(value, Tag->album);
        int res = addValue(&T[TagCount], "Album", value);

        estimatedbytes += res;
        if (res > 0)
            TagCount++;
        free(value);
    }

    if (strlen(Tag->comment) > 0) {
        char *value = (char *) malloc(strlen(Tag->comment) + 1);

        strcpy(value, Tag->comment);
        int res = addValue(&T[TagCount], "Comment", value);

        estimatedbytes += res;
        if (res > 0)
            TagCount++;
        free(value);
    }

    if (strlen(Tag->genre) > 0) {
        char *value = (char *) malloc(strlen(Tag->genre) + 1);

        strcpy(value, Tag->genre);
        int res = addValue(&T[TagCount], "Genre", value);

        estimatedbytes += res;
        if (res > 0)
            TagCount++;
        free(value);
    }

    if (strlen(Tag->track) > 0) {
        char *value = (char *) malloc(strlen(Tag->track) + 1);

        strcpy(value, Tag->track);
        int res = addValue(&T[TagCount], "Track", value);

        estimatedbytes += res;
        if (res > 0)
            TagCount++;
        free(value);
    }

    if (strlen(Tag->year) > 0) {
        char *value = (char *) malloc(strlen(Tag->year) + 1);

        strcpy(value, Tag->year);
        int res = addValue(&T[TagCount], "Year", value);

        estimatedbytes += res;
        if (res > 0)
            TagCount++;
        free(value);
    }
    // Start writing the new Ape2 Tag
    fseek(fp, 0L, SEEK_END);

    if (TagCount == 0) {
        printf("no tag to write");
        return 0;
    }

    if (estimatedbytes >= 8192 + 103) {
        printf
            ("\nTag is %.1f Kbyte long. This is longer than the maximum recommended 8 KByte.\n\a",
             estimatedbytes / 1024.);
        return 0;
    }

    H[8] = Version >> 0;
    H[9] = Version >> 8;
    H[10] = Version >> 16;
    H[11] = Version >> 24;
    H[12] = estimatedbytes >> 0;
    H[13] = estimatedbytes >> 8;
    H[14] = estimatedbytes >> 16;
    H[15] = estimatedbytes >> 24;
    H[16] = TagCount >> 0;
    H[17] = TagCount >> 8;
    H[18] = TagCount >> 16;
    H[19] = TagCount >> 24;

    H[23] = 0x80 | 0x20;
    writtenbytes += fwrite(H, 1, 32, fp);

    for (unsigned int i = 0; i < TagCount; i++) {
        dw[0] = T[i].valuelen >> 0;
        dw[1] = T[i].valuelen >> 8;
        dw[2] = T[i].valuelen >> 16;
        dw[3] = T[i].valuelen >> 24;
        dw[4] = T[i].flags >> 0;
        dw[5] = T[i].flags >> 8;
        dw[6] = T[i].flags >> 16;
        dw[7] = T[i].flags >> 24;
        writtenbytes += fwrite(dw, 1, 8, fp);
        writtenbytes += fwrite(T[i].key, 1, T[i].keylen, fp);
        writtenbytes += fwrite("", 1, 1, fp);
        if (T[i].valuelen > 0)
            writtenbytes += fwrite(T[i].value, 1, T[i].valuelen, fp);
    }

    H[23] = 0x80;
    writtenbytes += fwrite(H, 1, 32, fp);

    if (estimatedbytes != (unsigned long) writtenbytes)
        printf("\nError writing APE tag.\n");
    fclose(fp);
    TagCount = 0;
    return 0;
}
