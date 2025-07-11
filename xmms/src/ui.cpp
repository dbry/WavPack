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

void load_tag(ape_tag *tag, WavpackContext *ctx);
void update_tag(ape_tag *tag, char *filename);
void delete_tag(char *filename);
gboolean clipPreventionEnabled;
gboolean dynBitrateEnabled;
gboolean replaygainEnabled;
gboolean albumReplaygainEnabled;
gboolean openedAudio;
static GtkWidget *window = NULL;
static GtkWidget *title_entry;
static GtkWidget *album_entry;
static GtkWidget *performer_entry;
static GtkWidget *tracknumber_entry;
static GtkWidget *date_entry;
static GtkWidget *genre_entry;
static GtkWidget *user_comment_entry;
static char *filename;

void
wv_about_box()
{
    static GtkWidget *about_window;

    if (about_window)
        gdk_window_raise(about_window->window);

    about_window =
        xmms_show_message(g_strdup_printf
                          ("WavPack Decoder Plugin %s", VERSION),
                          ("Plugin code by \n" "Miles Egan and David Bryant\n"
                           "Adapted from xmms-musepack plugin by Lefungus\n"
                           "Visit the WavPack site at http://www.wavpack.com/\n"),
                          ("Ok"), FALSE, NULL, NULL);
    gtk_signal_connect(GTK_OBJECT(about_window), "destroy",
                       GTK_SIGNAL_FUNC(gtk_widget_destroyed), &about_window);
}

static void
label_set_text(GtkWidget * label, char *str, ...)
{
    va_list args;
    gchar *tempstr;

    va_start(args, str);
    tempstr = g_strdup_vprintf(str, args);
    va_end(args);

    gtk_label_set_text(GTK_LABEL(label), tempstr);
    g_free(tempstr);
}

static void
remove_cb(GtkWidget * w, gpointer data)
{
    delete_tag(filename);
    g_free(filename);
    gtk_widget_destroy(window);
}

static void
save_cb(GtkWidget * w, gpointer data)
{
    ape_tag Tag;

    strcpy(Tag.title, gtk_entry_get_text(GTK_ENTRY(title_entry)));
    strcpy(Tag.artist, gtk_entry_get_text(GTK_ENTRY(performer_entry)));
    strcpy(Tag.album, gtk_entry_get_text(GTK_ENTRY(album_entry)));
    strcpy(Tag.comment, gtk_entry_get_text(GTK_ENTRY(user_comment_entry)));
    strcpy(Tag.track, gtk_entry_get_text(GTK_ENTRY(tracknumber_entry)));
    strcpy(Tag.year, gtk_entry_get_text(GTK_ENTRY(date_entry)));
    strcpy(Tag.genre, gtk_entry_get_text(GTK_ENTRY(genre_entry)));
    update_tag (&Tag, filename);
    g_free(filename);
    gtk_widget_destroy(window);
}

static void
close_window(GtkWidget * w, gpointer data)
{
    g_free(filename);
    gtk_widget_destroy(window);
}

void
wv_file_info_box(char *fn)
{
    gchar *tmp;
    gint time, minutes, seconds;
    ape_tag tag;
    char    track_gain      [16];
    char    track_peak      [16];
    char    album_gain      [16];
    char    album_peak      [16];

    assert(fn != NULL);
    char error_buff[80];
    WavpackContext *ctx = WavpackOpenFileInput(fn, error_buff, OPEN_TAGS | OPEN_WVC, 0);
    if (ctx == NULL) {
        printf("wavpack: Error opening file: \"%s: %s\"\n", fn, error_buff);
        return;
    }
    int sample_rate = WavpackGetSampleRate(ctx);
    int num_channels = WavpackGetNumChannels(ctx);
    load_tag(&tag, ctx);
    WavpackGetTagItem(ctx, "replaygain_track_gain", track_gain, sizeof(track_gain));
    WavpackGetTagItem(ctx, "replaygain_track_peak", track_peak, sizeof(track_peak));
    WavpackGetTagItem(ctx, "replaygain_album_gain", album_gain, sizeof(album_gain));
    WavpackGetTagItem(ctx, "replaygain_album_peak", album_peak, sizeof(album_peak));
    DBG("opened %s at %d rate with %d channels\n", fn, sample_rate, num_channels);

    filename = g_strdup(fn);
    static GtkWidget *info_frame, *info_box, *bitrate_label, *rate_label;
    static GtkWidget *version_label, *bits_per_sample_label;
    static GtkWidget *channel_label, *length_label, *filesize_label;
    static GtkWidget *peakTitle_label, *peakAlbum_label, *gainTitle_label;
    static GtkWidget *gainAlbum_label, *filename_entry, *tag_frame;

    if (!window) {
        GtkWidget *hbox, *label, *filename_hbox, *vbox, *left_vbox;
        GtkWidget *table, *bbox, *cancel_button;
        GtkWidget *save_button, *remove_button;

        window = gtk_window_new(GTK_WINDOW_DIALOG);
        gtk_window_set_policy(GTK_WINDOW(window), FALSE, FALSE, FALSE);
        gtk_signal_connect(GTK_OBJECT(window), "destroy",
                           GTK_SIGNAL_FUNC(gtk_widget_destroyed), &window);
        gtk_container_set_border_width(GTK_CONTAINER(window), 10);

        vbox = gtk_vbox_new(FALSE, 10);
        gtk_container_add(GTK_CONTAINER(window), vbox);

        filename_hbox = gtk_hbox_new(FALSE, 5);
        gtk_box_pack_start(GTK_BOX(vbox), filename_hbox, FALSE, TRUE, 0);

        label = gtk_label_new("Filename:");
        gtk_box_pack_start(GTK_BOX(filename_hbox), label, FALSE, TRUE, 0);
        filename_entry = gtk_entry_new();
        gtk_editable_set_editable(GTK_EDITABLE(filename_entry), FALSE);
        gtk_box_pack_start(GTK_BOX(filename_hbox), filename_entry,
                           TRUE, TRUE, 0);

        hbox = gtk_hbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

        left_vbox = gtk_vbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(hbox), left_vbox, FALSE, FALSE, 0);

        tag_frame = gtk_frame_new("Ape2 Tag");
        gtk_box_pack_start(GTK_BOX(left_vbox), tag_frame, FALSE, FALSE, 0);

        table = gtk_table_new(5, 5, FALSE);
        gtk_container_set_border_width(GTK_CONTAINER(table), 5);
        gtk_container_add(GTK_CONTAINER(tag_frame), table);

        label = gtk_label_new("Title:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1,
                         GTK_FILL, GTK_FILL, 5, 5);

        title_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), title_entry, 1, 4, 0, 1,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Artist:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 1, 2,
                         GTK_FILL, GTK_FILL, 5, 5);

        performer_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), performer_entry, 1, 4, 1, 2,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Album:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 2, 3,
                         GTK_FILL, GTK_FILL, 5, 5);

        album_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), album_entry, 1, 4, 2, 3,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Comment:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 3, 4,
                         GTK_FILL, GTK_FILL, 5, 5);

        user_comment_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), user_comment_entry, 1, 4, 3,
                         4,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Year:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 4, 5,
                         GTK_FILL, GTK_FILL, 5, 5);

        date_entry = gtk_entry_new();
        gtk_widget_set_usize(date_entry, 60, -1);
        gtk_table_attach(GTK_TABLE(table), date_entry, 1, 2, 4, 5,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Track n°:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 2, 3, 4, 5,
                         GTK_FILL, GTK_FILL, 5, 5);

        tracknumber_entry = gtk_entry_new_with_max_length(4);
        gtk_widget_set_usize(tracknumber_entry, 20, -1);
        gtk_table_attach(GTK_TABLE(table), tracknumber_entry, 3, 4, 4,
                         5,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Genre:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 5, 6,
                         GTK_FILL, GTK_FILL, 5, 5);

        genre_entry = gtk_entry_new();
        gtk_widget_set_usize(genre_entry, 20, -1);
        gtk_table_attach(GTK_TABLE(table), genre_entry, 1, 4, 5,
                         6,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        bbox = gtk_hbutton_box_new();
        gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
        gtk_button_box_set_spacing(GTK_BUTTON_BOX(bbox), 5);
        gtk_box_pack_start(GTK_BOX(left_vbox), bbox, FALSE, FALSE, 0);

        save_button = gtk_button_new_with_label("Save");
        gtk_signal_connect(GTK_OBJECT(save_button), "clicked",
                           GTK_SIGNAL_FUNC(save_cb), NULL);
        GTK_WIDGET_SET_FLAGS(save_button, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(bbox), save_button, TRUE, TRUE, 0);

        remove_button = gtk_button_new_with_label("Remove Tag");
        gtk_signal_connect_object(GTK_OBJECT(remove_button),
                                  "clicked",
                                  GTK_SIGNAL_FUNC(remove_cb), NULL);
        GTK_WIDGET_SET_FLAGS(remove_button, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(bbox), remove_button, TRUE, TRUE, 0);

        cancel_button = gtk_button_new_with_label("Cancel");
        gtk_signal_connect_object(GTK_OBJECT(cancel_button),
                                  "clicked",
                                  GTK_SIGNAL_FUNC(close_window), NULL);
        GTK_WIDGET_SET_FLAGS(cancel_button, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(bbox), cancel_button, TRUE, TRUE, 0);
        gtk_widget_grab_default(cancel_button);

        info_frame = gtk_frame_new("WavPack Info:");
        gtk_box_pack_start(GTK_BOX(hbox), info_frame, FALSE, FALSE, 0);

        info_box = gtk_vbox_new(FALSE, 5);
        gtk_container_add(GTK_CONTAINER(info_frame), info_box);
        gtk_container_set_border_width(GTK_CONTAINER(info_box), 10);
        gtk_box_set_spacing(GTK_BOX(info_box), 0);

        version_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(version_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(version_label),
                              GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), version_label, FALSE,
                           FALSE, 0);

        bits_per_sample_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(bits_per_sample_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(bits_per_sample_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), bits_per_sample_label, FALSE, FALSE, 0);

        bitrate_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(bitrate_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(bitrate_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), bitrate_label, FALSE, FALSE, 0);

        rate_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(rate_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(rate_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), rate_label, FALSE, FALSE, 0);

        channel_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(channel_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(channel_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), channel_label, FALSE, FALSE, 0);

        length_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(length_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(length_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), length_label, FALSE, FALSE, 0);

        filesize_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(filesize_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(filesize_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), filesize_label, FALSE,
                           FALSE, 0);

        gainTitle_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(gainTitle_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(gainTitle_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), gainTitle_label, FALSE,
                           FALSE, 0);

        peakTitle_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(peakTitle_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(peakTitle_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), peakTitle_label, FALSE,
                           FALSE, 0);

        gainAlbum_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(gainAlbum_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(gainAlbum_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), gainAlbum_label, FALSE,
                           FALSE, 0);

        peakAlbum_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(peakAlbum_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(peakAlbum_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), peakAlbum_label, FALSE,
                           FALSE, 0);

        gtk_widget_show_all(window);
    }
    else
        gdk_window_raise(window->window);

    gtk_widget_set_sensitive(tag_frame, TRUE);

    gtk_label_set_text(GTK_LABEL(version_label), "");
    gtk_label_set_text(GTK_LABEL(bits_per_sample_label), "");
    gtk_label_set_text(GTK_LABEL(bitrate_label), "");
    gtk_label_set_text(GTK_LABEL(rate_label), "");
    gtk_label_set_text(GTK_LABEL(channel_label), "");
    gtk_label_set_text(GTK_LABEL(length_label), "");
    gtk_label_set_text(GTK_LABEL(filesize_label), "");
    gtk_label_set_text(GTK_LABEL(peakTitle_label), "");
    gtk_label_set_text(GTK_LABEL(peakAlbum_label), "");
    gtk_label_set_text(GTK_LABEL(gainTitle_label), "");
    gtk_label_set_text(GTK_LABEL(gainAlbum_label), "");

    time = WavpackGetNumSamples(ctx) / WavpackGetSampleRate(ctx);
    minutes = time / 60;
    seconds = time % 60;

    label_set_text(version_label, "Version: %d", WavpackGetVersion(ctx));
    label_set_text(bitrate_label, "Average bitrate: %6.1f kbps", WavpackGetAverageBitrate(ctx, 1) / 1000);
    label_set_text(rate_label, "Samplerate: %d Hz", WavpackGetSampleRate(ctx));
    label_set_text(bits_per_sample_label, "Bits per sample: %d", WavpackGetBitsPerSample(ctx));
    label_set_text(channel_label, "Channels: %d", WavpackGetNumChannels(ctx));
    label_set_text(length_label, "Length: %d:%.2d", minutes, seconds);
    label_set_text(filesize_label, "File size: %d Bytes", WavpackGetFileSize(ctx));

    label_set_text(gainTitle_label, "Title gain: %s", track_gain);
    label_set_text(peakTitle_label, "Title peak: %s", track_peak);
    label_set_text(gainAlbum_label, "Album gain: %s", album_gain);
    label_set_text(peakAlbum_label, "Album peak: %s", album_peak);
    
    gtk_entry_set_text(GTK_ENTRY(title_entry), tag.title);
    gtk_entry_set_text(GTK_ENTRY(performer_entry), tag.artist);
    gtk_entry_set_text(GTK_ENTRY(album_entry), tag.album);
    gtk_entry_set_text(GTK_ENTRY(user_comment_entry), tag.comment);
    gtk_entry_set_text(GTK_ENTRY(genre_entry), tag.genre);
    gtk_entry_set_text(GTK_ENTRY(tracknumber_entry), tag.track);
    gtk_entry_set_text(GTK_ENTRY(date_entry), tag.year);
    gtk_entry_set_text(GTK_ENTRY(filename_entry), fn);
    gtk_editable_set_position(GTK_EDITABLE(filename_entry), -1);

    WavpackCloseFile (ctx);
    tmp = g_strdup_printf("File Info - %s", g_basename(fn));
    gtk_window_set_title(GTK_WINDOW(window), tmp);
    g_free(tmp);
}

static GtkWidget *wv_configurewin = NULL;
static GtkWidget *vbox, *notebook;
static GtkWidget *rg_switch, *rg_clip_switch, *rg_track_gain, *rg_dyn_bitrate;

static void
wv_configurewin_ok(GtkWidget * widget, gpointer data)
{
    ConfigFile *cfg;
    GtkToggleButton *tb;

    tb = GTK_TOGGLE_BUTTON(rg_switch);
    replaygainEnabled = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(rg_clip_switch);
    clipPreventionEnabled = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(rg_dyn_bitrate);
    dynBitrateEnabled = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(rg_track_gain);
    albumReplaygainEnabled = !gtk_toggle_button_get_active(tb);

    cfg = xmms_cfg_open_default_file();

    xmms_cfg_write_boolean(cfg, "wavpack", "clip_prevention",
                           clipPreventionEnabled);
    xmms_cfg_write_boolean(cfg, "wavpack", "album_replaygain",
                           albumReplaygainEnabled);
    xmms_cfg_write_boolean(cfg, "wavpack", "dyn_bitrate", dynBitrateEnabled);
    xmms_cfg_write_boolean(cfg, "wavpack", "replaygain", replaygainEnabled);
    xmms_cfg_write_default_file(cfg);
    xmms_cfg_free(cfg);
    gtk_widget_destroy(wv_configurewin);
}

static void
rg_switch_cb(GtkWidget * w, gpointer data)
{
    gtk_widget_set_sensitive(GTK_WIDGET(data),
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON
                                                          (w)));
}

void
wv_configure(void)
{

    GtkWidget *rg_frame, *rg_vbox;
    GtkWidget *bbox, *ok, *cancel;
    GtkWidget *rg_type_frame, *rg_type_vbox, *rg_album_gain;

    if (wv_configurewin != NULL) {
        gdk_window_raise(wv_configurewin->window);
        return;
    }

    wv_configurewin = gtk_window_new(GTK_WINDOW_DIALOG);
    gtk_signal_connect(GTK_OBJECT(wv_configurewin), "destroy",
                       GTK_SIGNAL_FUNC(gtk_widget_destroyed),
                       &wv_configurewin);
    gtk_window_set_title(GTK_WINDOW(wv_configurewin),
                         "WavPack Configuration");
    gtk_window_set_policy(GTK_WINDOW(wv_configurewin), FALSE, FALSE, FALSE);
    gtk_container_border_width(GTK_CONTAINER(wv_configurewin), 10);

    vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_add(GTK_CONTAINER(wv_configurewin), vbox);

    notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);


    /* Plugin Settings */

    rg_frame = gtk_frame_new("General Plugin Settings:");
    gtk_container_border_width(GTK_CONTAINER(rg_frame), 5);

    rg_vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_border_width(GTK_CONTAINER(rg_vbox), 5);
    gtk_container_add(GTK_CONTAINER(rg_frame), rg_vbox);

    rg_dyn_bitrate =
        gtk_check_button_new_with_label("Enable Dynamic Bitrate Display");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_dyn_bitrate),
                                 dynBitrateEnabled);
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_dyn_bitrate, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), rg_frame,
                             gtk_label_new("Plugin"));

    /* Replay Gain.. */

    rg_frame = gtk_frame_new("ReplayGain Settings:");
    gtk_container_border_width(GTK_CONTAINER(rg_frame), 5);

    rg_vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_border_width(GTK_CONTAINER(rg_vbox), 5);
    gtk_container_add(GTK_CONTAINER(rg_frame), rg_vbox);

    rg_clip_switch =
        gtk_check_button_new_with_label("Enable Clipping Prevention");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_clip_switch),
                                 clipPreventionEnabled);
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_clip_switch, FALSE, FALSE, 0);

    rg_switch = gtk_check_button_new_with_label("Enable ReplayGain");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_switch),
                                 replaygainEnabled);
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_switch, FALSE, FALSE, 0);

    rg_type_frame = gtk_frame_new("ReplayGain Type:");
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_type_frame, FALSE, FALSE, 0);

    gtk_signal_connect(GTK_OBJECT(rg_switch), "toggled",
                       GTK_SIGNAL_FUNC(rg_switch_cb), rg_type_frame);

    rg_type_vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(rg_type_vbox), 5);
    gtk_container_add(GTK_CONTAINER(rg_type_frame), rg_type_vbox);

    rg_track_gain =
        gtk_radio_button_new_with_label(NULL, "use Track Gain/Peak");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_track_gain),
                                 !albumReplaygainEnabled);
    gtk_box_pack_start(GTK_BOX(rg_type_vbox), rg_track_gain, FALSE, FALSE, 0);

    rg_album_gain =
        gtk_radio_button_new_with_label(gtk_radio_button_group
                                        (GTK_RADIO_BUTTON(rg_track_gain)),
                                        "use Album Gain/Peak");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_album_gain),
                                 albumReplaygainEnabled);
    gtk_box_pack_start(GTK_BOX(rg_type_vbox), rg_album_gain, FALSE, FALSE, 0);

    gtk_widget_set_sensitive(rg_type_frame, replaygainEnabled);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), rg_frame,
                             gtk_label_new("ReplayGain"));

    /* Buttons */

    bbox = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
    gtk_button_box_set_spacing(GTK_BUTTON_BOX(bbox), 5);
    gtk_box_pack_start(GTK_BOX(vbox), bbox, FALSE, FALSE, 0);

    ok = gtk_button_new_with_label("Ok");
    gtk_signal_connect(GTK_OBJECT(ok), "clicked",
                       GTK_SIGNAL_FUNC(wv_configurewin_ok), NULL);
    GTK_WIDGET_SET_FLAGS(ok, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(bbox), ok, TRUE, TRUE, 0);
    gtk_widget_grab_default(ok);

    cancel = gtk_button_new_with_label("Cancel");
    gtk_signal_connect_object(GTK_OBJECT(cancel), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(wv_configurewin));
    GTK_WIDGET_SET_FLAGS(cancel, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(bbox), cancel, TRUE, TRUE, 0);

    gtk_widget_show_all(wv_configurewin);
}
