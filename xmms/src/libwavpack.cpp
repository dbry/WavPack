/************************************************************************************
    xmms-wavpack - input plugin for WavPack audio files in xmms
    Copyright (C) 2009 by Lefungus, Miles Egan, and David Bryant

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*************************************************************************************/

#ifndef ENABLE_LIBICONV
#define LIBICONV_PLUG 1
#endif

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <wavpack/wavpack.h>
extern "C" {
#include <xmms/plugin.h>
#include <xmms/configfile.h>
#include <xmms/titlestring.h>
#include <xmms/util.h>
}
#include <glib.h>
#include <gtk/gtk.h>
#include <iconv.h>
#include <math.h>
#include "equalizer.h"
#include "tags.h"
#ifndef M_LN10
#define M_LN10   2.3025850929940456840179914546843642
#endif

#define DBG(format, args...) fprintf(stderr, format, ## args)
#define BUFFER_SIZE 256 // read buffer size, in samples

extern "C" InputPlugin * get_iplugin_info(void);
static float calculate_gain (WavpackContext *wpc);
static void wv_load_config();
static int wv_is_our_file(char *);
static void wv_play(char *);
static void wv_stop(void);
static void wv_pause(short);
static void wv_seek(int);
static void wv_set_eq(int, float, float *);
static int wv_get_time(void);
static void wv_get_song_info(char *, char **, int *);
static char *generate_title(const char *, WavpackContext *ctx);
static int isSeek, paused;
static bool killDecodeThread;
static bool AudioError;
static pthread_t thread_handle;
static gboolean EQ_on;

// in ui.cpp
void wv_configure();
void wv_about_box(void);
void wv_file_info_box(char *);
extern gboolean clipPreventionEnabled;
extern gboolean dynBitrateEnabled;
extern gboolean replaygainEnabled;
extern gboolean albumReplaygainEnabled;
extern gboolean openedAudio;

InputPlugin mod = {
    NULL,                       //handle
    NULL,                       //filename
    NULL,
    wv_load_config,
    wv_about_box,
    wv_configure,
    wv_is_our_file,
    NULL,                       //no use
    wv_play,
    wv_stop,
    wv_pause,
    wv_seek,
    wv_set_eq,                 //set eq
    wv_get_time,
    NULL,                       //get volume
    NULL,                       //set volume
    NULL,                       //cleanup
    NULL,                       //obsolete
    NULL,                       //add_vis
    NULL,
    NULL,
    wv_get_song_info,
    wv_file_info_box,          //info box
    NULL,                       //output
};

class WavpackDecoder
{
public:
    InputPlugin *mod;
    int32_t *input;
    int16_t *output;
    int sample_rate;
    int num_channels;
    int bytes_per_sample;
    WavpackContext *ctx;
    char error_buff[80];
    float play_gain, shaping_error [8];

    WavpackDecoder(InputPlugin *mod) : mod(mod)
    {
        ctx = NULL;
        input = NULL;
        output = NULL;
    }

    ~WavpackDecoder()
    {
        if (input != NULL) {
            free(input);
            input = NULL;
        }
        if (output != NULL) {
            free(output);
            output = NULL;
        }
        if (ctx != NULL) {
            WavpackCloseFile(ctx);
            ctx = NULL;
        }
    }

    bool attach(const char *filename)
    {
        ctx = WavpackOpenFileInput(filename, error_buff, OPEN_TAGS | OPEN_WVC | OPEN_NORMALIZE, 0);

        if (ctx == NULL) {
            return false;
        }

        sample_rate = WavpackGetSampleRate(ctx);
        num_channels = WavpackGetNumChannels(ctx);
        bytes_per_sample = WavpackGetBytesPerSample(ctx);
        input = (int32_t *)calloc(BUFFER_SIZE, num_channels * sizeof(int32_t));
        output = (int16_t *)calloc(BUFFER_SIZE, num_channels * sizeof(int16_t));
        memset (shaping_error, 0, sizeof (shaping_error));
        mod->set_info(generate_title(filename, ctx),
                      (int) (WavpackGetNumSamples(ctx) / sample_rate) * 1000,
                      (int) WavpackGetAverageBitrate(ctx, true),
                      (int) sample_rate, num_channels);
        play_gain = calculate_gain (ctx);
        DBG("gain value = %g\n", play_gain);
        return true;
    }

    bool open_audio()
    {
        return mod->output->open_audio(FMT_S16_NE, sample_rate, num_channels);
    }

    void process_buffer(size_t num_samples)
    {
        int tsamples = num_samples * num_channels;

        if (!(WavpackGetMode (ctx) & MODE_FLOAT)) {
            float scaler = (float) (1.0 / ((unsigned int32_t) 1 << (bytes_per_sample * 8 - 1)));
            float *fptr = (float *) input;
            int32_t *lptr = input;
            int cnt = tsamples;

            while (cnt--)
                *fptr++ = *lptr++ * scaler;
        }

        if (play_gain != 1.0) {
            float *fptr = (float *) input;
            int cnt = tsamples;

            while (cnt--)
                *fptr++ *= play_gain;
        }

        if (tsamples) {
            float *fptr = (float *) input;
            short *sptr = (short *) output;
            int cnt = num_samples, ch;

            while (cnt--)
                for (ch = 0; ch < num_channels; ++ch) {
                    int dst;

                    *fptr -= shaping_error [ch];

                    if (*fptr >= 1.0)
                        dst = 32767;
                    else if (*fptr <= -1.0)
                        dst = -32768;
                    else
                        dst = (int) floor (*fptr * 32768.0);

                    shaping_error [ch] = (float)(dst / 32768.0 - *fptr++);
                    *sptr++ = dst;
                }
        }

        if (EQ_on)
            iir ((char *) output, tsamples * sizeof(int16_t));

        mod->add_vis_pcm(mod->output->written_time(), FMT_S16_NE, num_channels, tsamples * sizeof(int16_t), output);
        mod->output->write_audio(output, tsamples * sizeof(int16_t));
    }
};

extern "C" InputPlugin *
get_iplugin_info(void)
{
    mod.description =
        g_strdup_printf(("Wavpack Decoder Plugin %s"), VERSION);
    return &mod;
}

static int
wv_is_our_file(char *filename)
{
    char *ext;

    ext = strrchr(filename, '.');
    if (ext) {
        if (!strcasecmp(ext, ".wv")) {
            return TRUE;
        }
    }
    return FALSE;
}

void
load_tag(ape_tag *tag, WavpackContext *ctx) 
{
    memset(tag, 0, sizeof(ape_tag));
    WavpackGetTagItem(ctx, "Album", tag->album, sizeof(tag->album));
    WavpackGetTagItem(ctx, "Artist", tag->artist, sizeof(tag->artist));
    WavpackGetTagItem(ctx, "Comment", tag->comment, sizeof(tag->comment));
    WavpackGetTagItem(ctx, "Genre", tag->genre, sizeof(tag->genre));
    WavpackGetTagItem(ctx, "Title", tag->title, sizeof(tag->title));
    WavpackGetTagItem(ctx, "Track", tag->track, sizeof(tag->track));
    WavpackGetTagItem(ctx, "Year", tag->year, sizeof(tag->year));
}

void
update_tag(ape_tag *tag, char *filename) 
{
    WavpackContext *ctx;
    char error_buff [80];

    ctx = WavpackOpenFileInput (filename, error_buff, OPEN_TAGS | OPEN_EDIT_TAGS, 0);

    if (!ctx) {
        char text[256];

        sprintf(text, "File \"%s\" not found or is read protected!\n",
                filename);
        xmms_show_message("File-Error", (gchar *) text, "Ok", FALSE, NULL,
                          NULL);
        return;
    }

    if (strlen (tag->album))
        WavpackAppendTagItem (ctx, "Album", tag->album, strlen (tag->album));
    else
        WavpackDeleteTagItem (ctx, "Album");

    if (strlen (tag->artist))
        WavpackAppendTagItem (ctx, "Artist", tag->artist, strlen (tag->artist));
    else
        WavpackDeleteTagItem (ctx, "Artist");

    if (strlen (tag->comment))
        WavpackAppendTagItem (ctx, "Comment", tag->comment, strlen (tag->comment));
    else
        WavpackDeleteTagItem (ctx, "Comment");

    if (strlen (tag->genre))
        WavpackAppendTagItem (ctx, "Genre", tag->genre, strlen (tag->genre));
    else
        WavpackDeleteTagItem (ctx, "Genre");

    if (strlen (tag->title))
        WavpackAppendTagItem (ctx, "Title", tag->title, strlen (tag->title));
    else
        WavpackDeleteTagItem (ctx, "Title");

    if (strlen (tag->track))
        WavpackAppendTagItem (ctx, "Track", tag->track, strlen (tag->track));
    else
        WavpackDeleteTagItem (ctx, "Track");

    if (strlen (tag->year))
        WavpackAppendTagItem (ctx, "Year", tag->year, strlen (tag->year));
    else
        WavpackDeleteTagItem (ctx, "Year");

    if (!WavpackWriteTag (ctx)) {
        char text[256];

        sprintf(text, "Couldn't write tag to \"%s\"!\n",
                filename);
        xmms_show_message("File-Error", (gchar *) text, "Ok", FALSE, NULL,
                          NULL);
    }

    WavpackCloseFile (ctx);
}

void
delete_tag(char *filename) 
{
    WavpackContext *ctx;
    char error_buff [80];
    char text [256];

    ctx = WavpackOpenFileInput (filename, error_buff, OPEN_TAGS | OPEN_EDIT_TAGS, 0);

    if (!ctx) {
        sprintf(text, "File \"%s\" not found or is read protected!\n",
                filename);
        xmms_show_message("File-Error", (gchar *) text, "Ok", FALSE, NULL,
                          NULL);
        return;
    }

    while (WavpackGetTagItemIndexed (ctx, 0, text, sizeof (text)))
        WavpackDeleteTagItem (ctx, text);

    if (!WavpackWriteTag (ctx)) {
        char text[256];

        sprintf(text, "Couldn't write tag to \"%s\"!\n",
                filename);
        xmms_show_message("File-Error", (gchar *) text, "Ok", FALSE, NULL,
                          NULL);
    }

    WavpackCloseFile (ctx);
}

static char *
convertUTF8toLocale(char *utf8)
{
    // note - opens a new iconv descriptor for each call
    // will have to find a way to reuse the descriptor if this turns
    // out to be too slow
    iconv_t idesc = iconv_open("", "UTF-8");
    if (idesc == (iconv_t) -1) {
        perror("iconv_open failed");
        return g_strdup(utf8);
    }

    size_t in_left = strlen(utf8);
    size_t out_left = 2 * in_left + 1;
    char *buf = (char *)g_malloc(out_left);
#if 1
    char *in = utf8;
#else
    const char *in = (const char *) utf8;   // some systems (freeBSD?) require const here
#endif
    char *out = buf;

    memset(buf, 0, out_left);
    size_t err = iconv(idesc, &in, &in_left, &out, &out_left);
    iconv_close(idesc);
    return buf;
}

static void *
end_thread()
{
    pthread_exit(NULL);
    return 0;
}

static void
wv_set_eq(int on, float preamp_ctrl, float *eq_ctrl)
{
    EQ_on = on;
    init_iir(EQ_on, preamp_ctrl, eq_ctrl);
}

static void *
DecodeThread(void *a)
{
    ape_tag tag;
    char *filename = (char *) a;
    int bps_updateCounter = 0;
    int bps;
    int i;
    WavpackDecoder d(&mod);

    if (!d.attach(filename)) {
        printf("wavpack: Error opening file: \"%s\"\n", filename);
        killDecodeThread = true;
        return end_thread();
    }
    bps = WavpackGetBytesPerSample(d.ctx) * d.num_channels;
    DBG("reading %s at %d rate with %d channels\n", filename, d.sample_rate, d.num_channels);

    if (!d.open_audio()) {
        DBG("error opening xmms audio channel\n");
        killDecodeThread = true;
        AudioError = true;
        openedAudio = false;
    }
    else {
        DBG("opened xmms audio channel\n");
        openedAudio = true;
    }
    unsigned status;
    char *display = generate_title(filename, d.ctx);
    int length = (int) (1000 * WavpackGetNumSamples(d.ctx));

    while (!killDecodeThread) {
        if (isSeek != -1) {
            DBG("seeking to position %d\n", isSeek);
            WavpackSeekSample(d.ctx, isSeek * d.sample_rate);
            isSeek = -1;
        }
        if (paused == 0
            && (mod.output->buffer_free() >=
                (1152 * 2 *
                 (16 / 8)) << (mod.output->buffer_playing()? 1 : 0))) {
            status =
                WavpackUnpackSamples(d.ctx, d.input, BUFFER_SIZE);
            if (status == (unsigned) (-1)) {
                printf("wavpack: Error decoding file.\n");
                break;
            }
            else if (status == 0) {
                killDecodeThread = true;
                break;
            }
            else {
                d.process_buffer(status);
            }
        }
        else {
            xmms_usleep(10000);
        }
    }
    return end_thread();
}

static void
wv_play(char *filename)
{
    paused = 0;
    isSeek = -1;
    killDecodeThread = false;
    AudioError = false;
    pthread_create(&thread_handle, NULL, DecodeThread, (void *) filename);
    return;
}

static char *
generate_title(const char *fn, WavpackContext *ctx)
{
    static char *displaytitle = NULL;
    ape_tag tag;
    TitleInput *ti;

    ti = (TitleInput *) g_malloc0(sizeof(TitleInput));
    ti->__size = XMMS_TITLEINPUT_SIZE;
    ti->__version = XMMS_TITLEINPUT_VERSION;

    ti->file_name = g_strdup(g_basename(fn));
    ti->file_ext = "wv";

    load_tag(&tag, ctx);

    // xmms doesn't support unicode...
    ti->track_name = convertUTF8toLocale(tag.title);
    ti->performer = convertUTF8toLocale(tag.artist);
    ti->album_name = convertUTF8toLocale(tag.album);
    ti->date = convertUTF8toLocale(tag.year);
    ti->track_number = atoi(tag.track);
    if (ti->track_number < 0)
        ti->track_number = 0;
    ti->year = atoi(tag.year);
    if (ti->year < 0)
        ti->year = 0;
    ti->genre = convertUTF8toLocale(tag.genre);
    ti->comment = convertUTF8toLocale(tag.comment);

    displaytitle = xmms_get_titlestring(xmms_get_gentitle_format(), ti);
    if (!displaytitle || *displaytitle == '\0'
        || (strlen(tag.title) == 0 && strlen(tag.artist) == 0))
        displaytitle = ti->file_name;
    g_free(ti->track_name);
    g_free(ti->performer);
    g_free(ti->album_name);
    g_free(ti->genre);
    g_free(ti->comment);
    g_free(ti);

    return displaytitle;
}

static void
wv_get_song_info(char *filename, char **title, int *length)
{
    assert(filename != NULL);
    char error_buff[80];
    WavpackContext *ctx = WavpackOpenFileInput(filename, error_buff, OPEN_TAGS | OPEN_WVC, 0);
    if (ctx == NULL) {
        printf("wavpack: Error opening file: \"%s: %s\"\n", filename, error_buff);
        return;
    }
    int sample_rate = WavpackGetSampleRate(ctx);
    int num_channels = WavpackGetNumChannels(ctx);
    DBG("reading %s at %d rate with %d channels\n", filename, sample_rate, num_channels);

    *length = (int)(WavpackGetNumSamples(ctx) / sample_rate) * 1000,
    *title = generate_title(filename, ctx);
    DBG("title for %s = %s\n", filename, *title);
    WavpackCloseFile(ctx);
}

static int
wv_get_time(void)
{
    if (!mod.output)
        return -1;
    if (AudioError)
        return -2;
    if (killDecodeThread && !mod.output->buffer_playing())
        return -1;
    return mod.output->output_time();
}


static void
wv_seek(int sec)
{
    isSeek = sec;
    mod.output->flush((int) (1000 * isSeek));
}

static void
wv_pause(short pause)
{
    mod.output->pause(paused = pause);
}

static void
wv_stop(void)
{
    killDecodeThread = true;
    if (thread_handle != 0) {
        pthread_join(thread_handle, NULL);
        if (openedAudio) {
            mod.output->buffer_free();
            mod.output->close_audio();
        }
        openedAudio = false;
        if (AudioError)
            printf("Could not open Audio\n");
    }

}

static void
wv_load_config()
{
    ConfigFile *cfg;

    cfg = xmms_cfg_open_default_file();

    xmms_cfg_read_boolean(cfg, "wavpack", "clip_prevention",
                          &clipPreventionEnabled);
    xmms_cfg_read_boolean(cfg, "wavpack", "album_replaygain",
                          &albumReplaygainEnabled);
    xmms_cfg_read_boolean(cfg, "wavpack", "dyn_bitrate", &dynBitrateEnabled);
    xmms_cfg_read_boolean(cfg, "wavpack", "replaygain", &replaygainEnabled);
    xmms_cfg_free(cfg);

    openedAudio = false;
}

//////////////////////////////////////////////////////////////////////////////
// This function uses the ReplayGain mode selected by the user and the info //
// stored in the specified tag to determine the gain value used to play the //
// file. Note that the gain is in voltage scaling (not dB), so a value of   //
// 1.0 (not 0.0) is unity gain.                                             //
//////////////////////////////////////////////////////////////////////////////

static float calculate_gain (WavpackContext *wpc)
{
    if (replaygainEnabled) {
        float gain_value = 0.0, peak_value = 1.0;
        char value [32];

        if (albumReplaygainEnabled && WavpackGetTagItem (wpc, "replaygain_album_gain", value, sizeof (value))) {
            gain_value = (float) atof (value);

            if (WavpackGetTagItem (wpc, "replaygain_album_peak", value, sizeof (value)))
                peak_value = (float) atof (value);
        }
        else if (WavpackGetTagItem (wpc, "replaygain_track_gain", value, sizeof (value))) {
            gain_value = (float) atof (value);

            if (WavpackGetTagItem (wpc, "replaygain_track_peak", value, sizeof (value)))
                peak_value = (float) atof (value);
        }
        else
            return 1.0;

        // convert gain from dB to voltage (with +/- 20 dB limit)

        if (gain_value > 20.0)
            gain_value = 10.0;
        else if (gain_value < -20.0)
            gain_value = (float) 0.1;
        else
            gain_value = (float) pow (10.0, gain_value / 20.0);

        if (peak_value * gain_value > 1.0 && clipPreventionEnabled)
            gain_value = (float)(1.0 / peak_value);

        return gain_value;
    }
    else
        return 1.0;
}
