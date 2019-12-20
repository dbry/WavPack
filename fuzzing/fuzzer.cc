#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "wavpack.h"

using namespace std;

#define BUF_SAMPLES 1024

typedef struct {
    unsigned char ungetc_char, ungetc_flag;
    unsigned char *sptr, *dptr, *eptr;
    int64_t total_bytes_read;
} WavpackRawContext;

static int32_t raw_read_bytes (void *id, void *data, int32_t bcount)
{
    WavpackRawContext *rcxt = (WavpackRawContext *) id;
    unsigned char *outptr = (unsigned char *) data;

    while (bcount) {
        if (rcxt->ungetc_flag) {
            *outptr++ = rcxt->ungetc_char;
            rcxt->ungetc_flag = 0;
            bcount--;
        }
        else {
            size_t bytes_to_copy = rcxt->eptr - rcxt->dptr;

            if (!bytes_to_copy)
                break;

            if (bytes_to_copy > bcount)
                bytes_to_copy = bcount;

            memcpy (outptr, rcxt->dptr, bytes_to_copy);
            rcxt->total_bytes_read += bytes_to_copy;
            rcxt->dptr += bytes_to_copy;
            outptr += bytes_to_copy;
            bcount -= bytes_to_copy;
        }
    }

    return (int32_t)(outptr - (unsigned char *) data);
}

static int32_t raw_write_bytes (void *id, void *data, int32_t bcount)
{
    return 0;
}

static int64_t raw_get_pos (void *id)
{
    WavpackRawContext *rcxt = (WavpackRawContext *) id;
    return rcxt->dptr - rcxt->sptr;
}

static int raw_set_pos_abs (void *id, int64_t pos)
{
    WavpackRawContext *rcxt = (WavpackRawContext *) id;

    if (rcxt->sptr + pos < rcxt->sptr || rcxt->sptr + pos > rcxt->eptr)
        return 1;

    rcxt->dptr = rcxt->sptr + pos;
    return 0;
}

static int raw_set_pos_rel (void *id, int64_t delta, int mode)
{
    WavpackRawContext *rcxt = (WavpackRawContext *) id;
    unsigned char *ref = NULL;

    if (mode == SEEK_SET)
        ref = rcxt->sptr;
    else if (mode == SEEK_CUR)
        ref = rcxt->dptr;
    else if (mode == SEEK_END)
        ref = rcxt->eptr;

    if (ref + delta < rcxt->sptr || ref + delta > rcxt->eptr)
        return 1;

    rcxt->dptr = ref + delta;
    return 0;
}

static int raw_push_back_byte (void *id, int c)
{
    WavpackRawContext *rcxt = (WavpackRawContext *) id;
    rcxt->ungetc_char = c;
    rcxt->ungetc_flag = 1;
    return c;
}

static int64_t raw_get_length (void *id)
{
    WavpackRawContext *rcxt = (WavpackRawContext *) id;
    return rcxt->eptr - rcxt->sptr;
}

static int raw_can_seek (void *id)
{
    return 1;
}

static int raw_close_stream (void *id)
{
    return 0;
}

static WavpackStreamReader64 raw_reader = {
    raw_read_bytes, raw_write_bytes, raw_get_pos, raw_set_pos_abs, raw_set_pos_rel,
    raw_push_back_byte, raw_get_length, raw_can_seek, NULL, raw_close_stream
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static long long times_called, opens, seeks, samples_decoded, text_tags, binary_tags;
    WavpackRawContext raw_wv;
    WavpackContext *wpc;
    char error [80];
    int num_chans, bps, mode, qmode;
    int64_t total_samples;
    int retval = 0;

    times_called++;

    memset (&raw_wv, 0, sizeof (WavpackRawContext));
    raw_wv.dptr = raw_wv.sptr = (unsigned char *) data;
    raw_wv.eptr = raw_wv.dptr + size;
    wpc = WavpackOpenFileInputEx64 (&raw_reader, &raw_wv, NULL, error, OPEN_TAGS | OPEN_WRAPPER | OPEN_DSD_AS_PCM | OPEN_NO_CHECKSUM, 0);

    if (!wpc) {
        retval = 1;
        goto exit;
    }

    opens++;
    num_chans = WavpackGetNumChannels (wpc);
    total_samples = WavpackGetNumSamples64 (wpc);
    bps = WavpackGetBytesPerSample (wpc);
    qmode = WavpackGetQualifyMode (wpc);
    mode = WavpackGetMode (wpc);

    // Get all the metadata tags (text & binary)
    if (mode & MODE_VALID_TAG) {
        int num_binary_items = WavpackGetNumBinaryTagItems (wpc);
        int num_items = WavpackGetNumTagItems (wpc), i;

        for (i = 0; i < num_items; ++i) {
            int item_len, value_len, j;
            char *item, *value;

            item_len = WavpackGetTagItemIndexed (wpc, i, NULL, 0);
            item = (char *) malloc (item_len + 1);
            WavpackGetTagItemIndexed (wpc, i, item, item_len + 1);
            value_len = WavpackGetTagItem (wpc, item, NULL, 0);
            value = (char *) malloc (value_len + 1);
            WavpackGetTagItem (wpc, item, value, value_len + 1);
            text_tags++;
            free (value);
            free (item);
        }

        for (i = 0; i < num_binary_items; ++i) {
            int item_len, value_len;
            char *item, *value;

            item_len = WavpackGetBinaryTagItemIndexed (wpc, i, NULL, 0);
            item = (char *) malloc (item_len + 1);
            WavpackGetBinaryTagItemIndexed (wpc, i, item, item_len + 1);
            value_len = WavpackGetBinaryTagItem (wpc, item, NULL, 0);
            value = (char *) malloc (value_len);
            WavpackGetBinaryTagItem (wpc, item, value, value_len);
            binary_tags++;
            free (value);
            free (item);
        }
    }

    // Decode all
    if (num_chans && num_chans <= 256) {
        int32_t decoded_samples [BUF_SAMPLES * num_chans];
        int unpack_result;

        do {
            unpack_result = WavpackUnpackSamples (wpc, decoded_samples, BUF_SAMPLES);
            samples_decoded += unpack_result;
        } while (unpack_result);
    }

    // Seek to 1/3 of the way in plus 1000 samples (definitely not a block boundary)
    if (WavpackSeekSample64 (wpc, total_samples / 3 + 1000))
        ++seeks;

    WavpackCloseFile (wpc);

exit:
    if (!(times_called | 0x3FF))
        printf ("LLVMFuzzerTestOneInput(): %lld calls, %lld opens, %lld seeks, %lld samples, %lld text & %lld binary tags\n",
            times_called, opens, seeks, samples_decoded, text_tags, binary_tags);

    return retval;
}

#ifdef STAND_ALONE_LENGTH   // max file length for stand-alone testing (sans fuzz)

int main (int argc, char **argv)
{
    unsigned char *buffer = (unsigned char *) malloc (STAND_ALONE_LENGTH);
    const char *filename = argc > 1 ? argv [1] : "testfile.wv";
    FILE *infile = fopen (filename, "rb");
    int retval, count = 1;
    int bytes_read;

    if (!infile) {
        fprintf (stderr, "can't open file %s!\n", filename);
        return 1;
    }

    if (argc > 2) {
        count = atol (argv [2]);

        if (!count) {
            fprintf (stderr, "second parameter is count!\n");
            return 1;
        }
    }

    bytes_read = fread (buffer, 1, STAND_ALONE_LENGTH, infile);
    printf ("read %d bytes from file %s, process count = %d\n", bytes_read, filename, count);

    if (bytes_read == STAND_ALONE_LENGTH)
        printf ("warning: at maximum length, perhaps truncated!\n");

    fclose (infile);

    while (count--)
        retval = LLVMFuzzerTestOneInput(buffer, bytes_read);

    printf ("retval = %d\n", retval);

    free (buffer);

    return 0;
}

#endif
