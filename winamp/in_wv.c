/*
** .WV input plug-in for WavPack
** Copyright (c) 2000 - 2006, Conifer Software, All Rights Reserved
*/

#include <windows.h>
#include <stdio.h>
#include <mmreg.h>
#include <msacm.h>
#include <math.h>

#include "in2.h"
#include "wavpack.h"
#include "resource.h"

int GetBitsPerSample (WavpackContext *wpc);

static float calculate_gain (WavpackContext *wpc, int *pSoftClip);

//#define DEBUG_CONSOLE

// use CRT. Good. Useful. Portable.
BOOL WINAPI DllMain (HANDLE hInst, ULONG ul_reason_for_call, LPVOID lpReserved)
{
    return TRUE;
}

// post this to the main window at end of file (after playback as stopped)
#define WM_WA_MPEG_EOF WM_USER+2

#define MAX_NCH 2
#define MAX_BPS 32

//#define BPS (WavHdr.BitsPerSample > 16 ? 32 : 16)
//#define NCH (WavHdr.NumChannels)

//#define BPS ((wpc && (wpc->wphdr.flags & BYTES_3)) ? 32 : 16)
//#define NCH ((wpc && (wpc->wphdr.flags & MONO_FLAG)) ? 1 : 2)

#define BPS (WavpackGetBitsPerSample (wpc) <= 16 ? 16 : 32)
#define NCH (WavpackGetReducedChannels (wpc))
#define SAMPRATE (WavpackGetSampleRate (wpc))

WavpackContext *wpc;

In_Module mod;          // the output module (declared near the bottom of this file)
char lastfn[MAX_PATH];  // currently playing file (used for getting info on the current file)
int decode_pos_ms;      // current decoding position, in milliseconds
int paused;             // are we paused?
int seek_needed;        // if != -1, it is the point that the decode thread should seek to, in ms.
char sample_buffer[576*MAX_NCH*(MAX_BPS/8)*2];  // sample buffer

#define IGNORE_WVC              0x1
#define REPLAYGAIN_TRACK        0x2
#define REPLAYGAIN_ALBUM        0x4
#define SOFTEN_CLIPPING         0x8
#define PREVENT_CLIPPING        0x10

int config_bits;        // all configuration goes here

//WaveHeader WavHdr;    // standard wav header for sample rate, etc.
//WavpackHeader WvpkHdr;// WavPack header for version, mode, etc.
//M_Tag m_tag;          // ID3v1 or APEv2 Tag (if present)

float play_gain;        // playback gain (for replaygain support)
int soft_clipping;      // soft clipping active for playback

//HANDLE input_file=INVALID_HANDLE_VALUE;       // input file handle
//HANDLE wvc_file=INVALID_HANDLE_VALUE; // wvc file handle

int killDecodeThread=0;                         // the kill switch for the decode thread
HANDLE thread_handle=INVALID_HANDLE_VALUE;      // the handle to the decode thread

DWORD WINAPI __stdcall DecodeThread(void *b);   // the decode thread procedure

static BOOL CALLBACK WavPackDlgProc (HWND, UINT, WPARAM, LPARAM);

void config (HWND hwndParent)
{
    char dllname [512];
    int temp_config;
    HMODULE module;
    HANDLE confile;
    DWORD result;

    module = GetModuleHandle ("in_wv.dll");
    temp_config = (int) DialogBoxParam (module, "WinAmp", hwndParent, (DLGPROC) WavPackDlgProc, config_bits);

    if (temp_config == config_bits || (temp_config & 0xffffffe0) ||
        (temp_config & 6) == 6 || (temp_config & 0x18) == 0x18)
            return;

    config_bits = temp_config;

    if (module && GetModuleFileName (module, dllname, sizeof (dllname))) {
        dllname [strlen (dllname) - 2] = 'a';
        dllname [strlen (dllname) - 1] = 't';

        confile = CreateFile (dllname, GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (confile == INVALID_HANDLE_VALUE)
            return;

        WriteFile (confile, &config_bits, sizeof (config_bits), &result, NULL);
        CloseHandle (confile);
    }
}

BOOL CALLBACK WavPackDlgProc (HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    static int local_config;

    switch (message) {
        case WM_INITDIALOG:
            local_config = (int) lParam;

            CheckDlgButton (hDlg, IDC_USEWVC, local_config & IGNORE_WVC);

            SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_ADDSTRING, 0, (LPARAM) "disabled");
            SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_ADDSTRING, 0, (LPARAM) "use track gain");
            SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_ADDSTRING, 0, (LPARAM) "use album gain");
            SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_SETCURSEL, (local_config >> 1) & 3, 0);

            SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_ADDSTRING, 0, (LPARAM) "just clip peaks");
            SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_ADDSTRING, 0, (LPARAM) "softly clip peaks");
            SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_ADDSTRING, 0, (LPARAM) "scale track to prevent clips");
            SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_SETCURSEL, (local_config >> 3) & 3, 0);

            if (!(local_config & (REPLAYGAIN_TRACK | REPLAYGAIN_ALBUM))) {
                EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING_TEXT), FALSE);
                EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING), FALSE);
            }

            SetFocus (GetDlgItem (hDlg, IDC_USEWVC));
            return FALSE;

        case WM_COMMAND:
            switch (LOWORD (wParam)) {
                case IDC_REPLAYGAIN:
                    if (SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_GETCURSEL, 0, 0)) {
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING_TEXT), TRUE);
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING), TRUE);
                    }
                    else {
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING_TEXT), FALSE);
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING), FALSE);
                    }

                    break;

                case IDOK:
                    local_config = 0;

                    if (IsDlgButtonChecked (hDlg, IDC_USEWVC))
                        local_config |= IGNORE_WVC;

                    local_config |= SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_GETCURSEL, 0, 0) << 1;
                    local_config |= SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_GETCURSEL, 0, 0) << 3;

                case IDCANCEL:
                    EndDialog (hDlg, local_config);
                    return TRUE;
            }

            break;
    }

    return FALSE;
}

extern long dump_alloc (void);

void about (HWND hwndParent)
{
#ifdef DEBUG_ALLOC
    char string [80];
    sprintf (string, "alloc_count = %d", dump_alloc ());
    MessageBox (hwndParent, string, "About WavPack Player", MB_OK);
#else
    MessageBox (hwndParent,"WavPack Player Version 2.4 \nCopyright (c) 2006 Conifer Software ", "About WavPack Player", MB_OK);
#endif
}

void init() { /* any one-time initialization goes here (configuration reading, etc) */
    char dllname [512];
    HMODULE module;
    HANDLE confile;
    DWORD result;

    module = GetModuleHandle ("in_wv.dll");
    config_bits = 0;

    if (module && GetModuleFileName (module, dllname, sizeof (dllname))) {
        dllname [strlen (dllname) - 2] = 'a';
        dllname [strlen (dllname) - 1] = 't';

        confile = CreateFile (dllname, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (confile == INVALID_HANDLE_VALUE)
            return;

        if (!ReadFile (confile, &config_bits, sizeof (config_bits), &result, NULL) ||
            result != sizeof (config_bits))
                config_bits = 0;

        CloseHandle (confile);
    }
}

#ifdef DEBUG_CONSOLE

HANDLE debug_console=INVALID_HANDLE_VALUE;      // debug console

void debug_write (char *str)
{
    static int cant_debug;

    if (cant_debug)
        return;

    if (debug_console == INVALID_HANDLE_VALUE) {
        AllocConsole ();

#if 1
        debug_console = GetStdHandle (STD_OUTPUT_HANDLE);
#else
        debug_console = CreateConsoleScreenBuffer (GENERIC_WRITE, FILE_SHARE_WRITE,
            NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
#endif

        if (debug_console == INVALID_HANDLE_VALUE) {
            MessageBox(NULL, "Can't get a console handle", "WavPack",MB_OK);
            cant_debug = 1;
            return;
        }
        else if (!SetConsoleActiveScreenBuffer (debug_console)) {
            MessageBox(NULL, "Can't activate console buffer", "WavPack",MB_OK);
            cant_debug = 1;
            return;
        }
    }

    WriteConsole (debug_console, str, strlen (str), NULL, NULL);
}

#endif

void quit() { /* one-time deinit, such as memory freeing */
#ifdef DEBUG_CONSOLE
    if (debug_console != INVALID_HANDLE_VALUE) {
        FreeConsole ();

        if (debug_console != GetStdHandle (STD_OUTPUT_HANDLE))
            CloseHandle (debug_console);

        debug_console = INVALID_HANDLE_VALUE;
    }
#endif
}

int isourfile(char *fn)
{
    return 0;
}
// used for detecting URL streams.. unused here. strncmp(fn,"http://",7) to detect HTTP streams, etc

int play (char *fn)
{
    char error [128];
    int maxlatency;
    int thread_id;

#ifdef DEBUG_CONSOLE
    sprintf (error, "play (%s)\n", fn);
    debug_write (error);
#endif

    wpc = WavpackOpenFileInput (fn, error, (config_bits & OPEN_WVC) | OPEN_TAGS | OPEN_2CH_MAX | OPEN_NORMALIZE, 23);

    if (!wpc)           // error opening file, just return error
        return -1;

    play_gain = calculate_gain (wpc, &soft_clipping);
    strcpy (lastfn, fn);

    paused = 0;
    decode_pos_ms = 0;
    seek_needed = -1;

    maxlatency = mod.outMod->Open (SAMPRATE, NCH, BPS, -1, -1);

    if (maxlatency < 0) { // error opening device
        wpc = WavpackCloseFile (wpc);
        return -1;
    }

    // dividing by 1000 for the first parameter of setinfo makes it
    // display 'H'... for hundred.. i.e. 14H Kbps.

    mod.SetInfo (0, (SAMPRATE + 500) / 1000, NCH, 1);

    // initialize vis stuff

    mod.SAVSAInit (maxlatency, SAMPRATE);
    mod.VSASetInfo (SAMPRATE, NCH);

    mod.outMod->SetVolume (-666);       // set the output plug-ins default volume

    killDecodeThread=0;

    thread_handle = (HANDLE) CreateThread (NULL, 0, (LPTHREAD_START_ROUTINE) DecodeThread,
        (void *) &killDecodeThread, 0, &thread_id);

    if (SetThreadPriority (thread_handle, THREAD_PRIORITY_HIGHEST) == 0) {
        wpc = WavpackCloseFile (wpc);
        return -1;
    }

    return 0;
}

void pause ()
{
#ifdef DEBUG_CONSOLE
    debug_write ("pause ()\n");
#endif

    paused = 1;
    mod.outMod->Pause (1);
}

void unpause ()
{
#ifdef DEBUG_CONSOLE
    debug_write ("unpause ()\n");
#endif

    paused = 0;
    mod.outMod->Pause (0);
}

int ispaused ()
{
    return paused;
}

void stop()
{
#ifdef DEBUG_CONSOLE
    debug_write ("stop ()\n");
#endif

    if (thread_handle != INVALID_HANDLE_VALUE) {

        killDecodeThread = 1;

        if (WaitForSingleObject (thread_handle, INFINITE) == WAIT_TIMEOUT) {
            MessageBox(mod.hMainWindow,"error asking thread to die!\n", "error killing decode thread", 0);
            TerminateThread(thread_handle,0);
        }

        CloseHandle (thread_handle);
        thread_handle = INVALID_HANDLE_VALUE;
    }

    if (wpc)
        wpc = WavpackCloseFile (wpc);

    mod.outMod->Close ();
    mod.SAVSADeInit ();
}

int getlength()
{
    return (int)(WavpackGetNumSamples (wpc) * 1000.0 / SAMPRATE);
}

int getoutputtime()
{
    return decode_pos_ms + (mod.outMod->GetOutputTime () - mod.outMod->GetWrittenTime ());
}

void setoutputtime (int time_in_ms)
{
#ifdef DEBUG_CONSOLE
    char str [40];
    sprintf (str, "setoutputtime (%d)\n", time_in_ms);
    debug_write (str);
#endif

    seek_needed = time_in_ms;
}

void setvolume (int volume)
{
    mod.outMod->SetVolume (volume);
}

void setpan (int pan)
{
    mod.outMod->SetPan(pan);
}

static UTF8ToAnsi (char *string, int len);

int infoDlg (char *fn, HWND hwnd)
{
    char string [2048], chan_string [20], modes [80];
    WavpackContext *wpc;
    uchar md5_sum [16];

    wpc = WavpackOpenFileInput (fn, string, (config_bits & OPEN_WVC) | OPEN_TAGS | OPEN_2CH_MAX, 0);

    if (wpc) {
        int mode = WavpackGetMode (wpc);

        sprintf (string, "Encoder version:  %d\n", WavpackGetVersion (wpc));

        if (WavpackGetNumChannels (wpc) > 2)
            sprintf (chan_string, "%d (multichannel)", WavpackGetNumChannels (wpc));
        else
            strcpy (chan_string, WavpackGetNumChannels (wpc) == 1 ? "1 (mono)" : "2 (stereo)");

        sprintf (string + strlen (string), "Source:  %d-bit %s at %d Hz \n", WavpackGetBitsPerSample (wpc),
            (WavpackGetMode (wpc) & MODE_FLOAT) ? "floats" : "ints", SAMPRATE);

        sprintf (string + strlen (string), "Channels:  %s\n", chan_string);

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

        sprintf (string + strlen (string), "Modes:  %s\n", modes);

        if (WavpackGetRatio (wpc) != 0.0) {
            sprintf (string + strlen (string), "Average bitrate:  %d kbps \n", (int) ((WavpackGetAverageBitrate (wpc, TRUE) + 500.0) / 1000.0));
            sprintf (string + strlen (string), "Overall ratio:  %.2f to 1 \n", 1.0 / WavpackGetRatio (wpc));
        }

        if (WavpackGetMD5Sum (wpc, md5_sum)) {
            int i;

            strcat (string, "Original md5:  ");

            for (i = 0; i < 16; ++i)
                sprintf (string + strlen (string), "%02x", md5_sum [i]);

            strcat (string, " \n");
        }

        if (WavpackGetMode (wpc) & MODE_VALID_TAG) {
            char value [128];

            if (config_bits & (REPLAYGAIN_TRACK | REPLAYGAIN_ALBUM)) {
                int local_clipping;
                float local_gain;

                local_gain = calculate_gain (wpc, &local_clipping);

                if (local_gain != 1.0)
                    sprintf (string + strlen (string), "Gain:  %+.2f dB %s\n",
                        log10 (local_gain) * 20.0, local_clipping ? "(w/soft clipping)" : "");
            }

            if (WavpackGetTagItem (wpc, "title", value, sizeof (value))) {
                if (mode & MODE_APETAG)
                     UTF8ToAnsi (value, sizeof (value));

                sprintf (string + strlen (string), "\nTitle:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "artist", value, sizeof (value))) {
                if (mode & MODE_APETAG)
                     UTF8ToAnsi (value, sizeof (value));

                sprintf (string + strlen (string), "\nArtist:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "album", value, sizeof (value))) {
                if (mode & MODE_APETAG)
                     UTF8ToAnsi (value, sizeof (value));

                sprintf (string + strlen (string), "\nAlbum:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "comment", value, sizeof (value))) {
                if (mode & MODE_APETAG)
                     UTF8ToAnsi (value, sizeof (value));

                sprintf (string + strlen (string), "\nComment:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "year", value, sizeof (value)))
                sprintf (string + strlen (string), "\nYear:  %s", value);

            if (WavpackGetTagItem (wpc, "track", value, sizeof (value)))
                sprintf (string + strlen (string), "\nTrack:  %s", value);

            strcat (string, "\n");
        }

        MessageBox (hwnd, string, "WavPack File Info Box", MB_OK);
        wpc = WavpackCloseFile (wpc);
    }
    else
        MessageBox (hwnd, string, "WavPack Player", MB_OK);

    return 0;
}

void getfileinfo (char *filename, char *title, int *length_in_ms)
{
    if (!filename || !*filename) {      // currently playing file

        if (length_in_ms)
            *length_in_ms = getlength ();

        if (title) {
            if (WavpackGetTagItem (wpc, "title", NULL, 0)) {
                char art [128], ttl [128];

                WavpackGetTagItem (wpc, "title", ttl, sizeof (ttl));

                if (WavpackGetMode (wpc) & MODE_APETAG)
                     UTF8ToAnsi (ttl, sizeof (ttl));

                if (WavpackGetTagItem (wpc, "artist", art, sizeof (art))) {
                    if (WavpackGetMode (wpc) & MODE_APETAG)
                        UTF8ToAnsi (art, sizeof (art));

                    sprintf (title, "%s - %s", art, ttl);
                }
                else
                    strcpy (title, ttl);
            }
            else {
                char *p = lastfn + strlen (lastfn);

                while (*p != '\\' && p >= lastfn)
                    p--;

                strcpy(title,++p);
            }
        }
    }
    else {      // some other file
        WavpackContext *wpc;
        char error [128];

        if (length_in_ms)
            *length_in_ms = -1000;

        if (title)
            *title = 0;

        wpc = WavpackOpenFileInput (filename, error, (config_bits & OPEN_WVC) | OPEN_TAGS | OPEN_2CH_MAX, 0);

        if (wpc) {
            if (length_in_ms)
                *length_in_ms = (int)(WavpackGetNumSamples (wpc) * 1000.0 / SAMPRATE);

            if (title && WavpackGetTagItem (wpc, "title", NULL, 0)) {
                char art [128], ttl [128];

                WavpackGetTagItem (wpc, "title", ttl, sizeof (ttl));

                if (WavpackGetMode (wpc) & MODE_APETAG)
                     UTF8ToAnsi (ttl, sizeof (ttl));

                if (WavpackGetTagItem (wpc, "artist", art, sizeof (art))) {
                    if (WavpackGetMode (wpc) & MODE_APETAG)
                        UTF8ToAnsi (art, sizeof (art));

                    sprintf (title, "%s - %s", art, ttl);
                }
                else
                    strcpy (title, ttl);
            }

            wpc = WavpackCloseFile (wpc);
        }
        else
            MessageBox (NULL, error, "WavPack Player", MB_OK);


        if (title && !*title) {
            char *p = filename + strlen (filename);

            while (*p != '\\' && p >= filename) p--;
            strcpy(title,++p);
        }
    }
}

void eq_set (int on, char data [10], int preamp)
{
        // most plug-ins can't even do an EQ anyhow.. I'm working on writing
        // a generic PCM EQ, but it looks like it'll be a little too CPU
        // consuming to be useful :)
}

#define WORDS_ONLY

static void format_samples (uchar *dst, int bps, long *src, unsigned long samcnt)
{
    long temp;

    switch (bps) {
        case 1:
            while (samcnt--) {
                temp = *src++;
                dst [0] = 0;
                dst [1] = (uchar)temp;
                dst += 2;
            }

            break;

        case 2:
            while (samcnt--) {
                dst [0] = (uchar)(temp = *src++);
                dst [1] = (uchar)(temp >> 8);
                dst += 2;
            }

            break;

        case 3:
            while (samcnt--) {
                temp = *src++;
                dst [0] = 0;
                dst [1] = (uchar)temp;
                dst [2] = (uchar)(temp >> 8);
                dst [3] = (uchar)(temp >> 16);
                dst += 4;
            }

            break;

        case 4:
            while (samcnt--) {
                dst [0] = (uchar)(temp = *src++);
                dst [1] = (uchar)(temp >> 8);
                dst [2] = (uchar)(temp >> 16);
                dst [3] = (uchar)(temp >> 24);
                dst += 4;
            }

            break;
    }
}

DWORD WINAPI __stdcall DecodeThread (void *b)
{
    int done = 0;

    while (!*((int *)b) ) {

        if (seek_needed != -1) {
            int seek_position = seek_needed;
            int bc = 0;

            seek_needed = -1;

            if (seek_position > getlength () - 1000 && getlength () > 1000)
                seek_position = getlength () - 1000; // don't seek to last second

            mod.outMod->Flush (decode_pos_ms = seek_position);

            if (WavpackSeekSample (wpc, (int)(SAMPRATE / 1000.0 * seek_position))) {
                decode_pos_ms = (int)(WavpackGetSampleIndex (wpc) * 1000.0 / SAMPRATE);
                mod.outMod->Flush (decode_pos_ms);
                continue;
            }
            else
                done = 1;
        }

        if (done) {
            mod.outMod->CanWrite ();

            if (!mod.outMod->IsPlaying ()) {
                PostMessage (mod.hMainWindow, WM_WA_MPEG_EOF, 0, 0);
                return 0;
            }

            Sleep (10);
        }
        else if (mod.outMod->CanWrite() >= ((576 * NCH * (BPS / 8)) << (mod.dsp_isactive () ? 1 : 0))) {
            int tsamples = WavpackUnpackSamples (wpc, (long *) sample_buffer, 576) * NCH;
            int tbytes = tsamples * (BPS/8);

            if (!tsamples)
                done = 1;
            else {
                format_samples ((uchar*) sample_buffer, WavpackGetBytesPerSample (wpc), (long *) sample_buffer, tsamples);

                if (BPS == 32) {
                    if (WavpackGetMode (wpc) & MODE_FLOAT) {
                        long *ptr = (long *) sample_buffer;
                        int fcnt = tsamples;

                        while (fcnt--) {
                            *ptr = (long) *(float *) ptr;

                            if (*ptr > 8388607)
                                *ptr++ = 8388607 << 8;
                            else if (*ptr < -8388608)
                                *ptr = -8388608 << 8;
                            else
                                *ptr++ <<= 8;
                        }
                    }

                    // check for and handle required gain for 24-bit data

                    if (play_gain != 1.0) {
                        int *gain_ptr = (int *) sample_buffer;
                        int gain_cnt = tsamples;

                        if (play_gain > 1.0)
                            while (gain_cnt--) {
                                float value = (float)(*gain_ptr * play_gain * (1/256.0));

                                if (soft_clipping && value > 6291456.0)
                                    *gain_ptr = (int)(8388608.0 - (4398046511104.0 / (value - 4194304.0)));
                                else if (soft_clipping && value < -6291456.0)
                                    *gain_ptr = (int)(-8388608.0 - (4398046511104.0 / (value + 4194304.0)));
                                else if (value > 8388607.0)
                                    *gain_ptr = 8388607;
                                else if (value < -8388608.0)
                                    *gain_ptr = -8388608;
                                else
                                    *gain_ptr = (int) floor (value + 0.5);

                                *gain_ptr++ <<= 8;
                            }
                        else
                            while (gain_cnt--) {
                                float value = (float)(*gain_ptr * play_gain * (1/256.0));
                                *gain_ptr++ = (int) floor (value + 0.5) << 8;
                            }
                    }
                }
                else if (play_gain != 1.0) {
                    short *gain_ptr = (short *) sample_buffer;
                    int gain_cnt = tsamples;

                    if (play_gain > 1.0)
                        while (gain_cnt--) {
                            float value = *gain_ptr * play_gain;

                            if (soft_clipping && value > 24576.0)
                                *gain_ptr++ = (short)(32768.0 - (67108864.0 / (value - 16384.0)));
                            else if (soft_clipping && value < -24576.0)
                                *gain_ptr++ = (short)(-32768.0 - (67108864.0 / (value + 16384.0)));
                            else if (value > 32767.0)
                                *gain_ptr++ = 32767;
                            else if (value < -32768.0)
                                *gain_ptr++ = -32768;
                            else
                                *gain_ptr++ = (short) floor (value + 0.5);
                        }
                    else
                        while (gain_cnt--) {
                            float value = *gain_ptr * play_gain;
                            *gain_ptr++ = (short) floor (value + 0.5);
                        }
                }

                mod.SAAddPCMData ((char *) sample_buffer, NCH, BPS, decode_pos_ms);
                mod.VSAAddPCMData ((char *) sample_buffer, NCH, BPS, decode_pos_ms);
                decode_pos_ms = (int)(WavpackGetSampleIndex (wpc) * 1000.0 / SAMPRATE);

                if (mod.dsp_isactive())
                    tbytes = mod.dsp_dosamples ((short *) sample_buffer,
                        tsamples / NCH, BPS, NCH, SAMPRATE) * (NCH * (BPS/8));

                mod.outMod->Write (sample_buffer, tbytes);
            }
        }
        else {
            mod.SetInfo ((int) ((WavpackGetInstantBitrate (wpc) + 500.0) / 1000.0), -1, -1, 1);
            Sleep(20);
        }
    }

    return 0;
}



In_Module mod =
{
    IN_VER,
    "WavPack Player v2.4 "

#ifdef __alpha
    "(AXP)"
#else
    "(x86)"
#endif
    ,
    0,          // hMainWindow
    0,          // hDllInstance
    "WV\0WavPack File (*.WV)\0"
    ,
    1,          // is_seekable
    1,          // uses output
    config,
    about,
    init,
    quit,
    getfileinfo,
    infoDlg,
    isourfile,
    play,
    pause,
    unpause,
    ispaused,
    stop,
    getlength,
    getoutputtime,
    setoutputtime,
    setvolume,
    setpan,
    0,0,0,0,0,0,0,0,0,  // vis stuff
    0,0,                // dsp
    eq_set,
    NULL,               // setinfo
    0                   // out_mod
};

__declspec (dllexport) In_Module * winampGetInModule2 ()
{
    return &mod;
}

typedef struct {
    char *filename;
    char *metadata;
    char *ret;
    int retlen;
} extendedFileInfoStruct;

__declspec (dllexport) int winampGetExtendedFileInfo (extendedFileInfoStruct exfinfo)
{
    WavpackContext *wpc;
    char error [128];
    int retval = 0;

    if (!exfinfo.filename || !*exfinfo.filename)
        return retval;

    wpc = WavpackOpenFileInput (exfinfo.filename, error, OPEN_TAGS, 0);

    if (wpc) {
        if (!_stricmp (exfinfo.metadata, "length")) {
            char string [20];

            sprintf (string, "%d", (int)(WavpackGetNumSamples (wpc) * 1000.0 / WavpackGetSampleRate (wpc)));

            if (strlen (string) < (uint) exfinfo.retlen) {
                strcpy (exfinfo.ret, string);
                retval = 1;
            }
        }
        else if (WavpackGetTagItem (wpc, exfinfo.metadata, exfinfo.ret, exfinfo.retlen)) {
            if (WavpackGetMode (wpc) & MODE_APETAG)
                UTF8ToAnsi (exfinfo.ret, exfinfo.retlen);

            retval = 1;
        }
    }

    if (wpc)
        WavpackCloseFile (wpc);

    return retval;
}

//////////////////////////////////////////////////////////////////////////////
// This function uses the ReplayGain mode selected by the user and the info //
// stored in the specified tag to determine the gain value used to play the //
// file and whether "soft clipping" is required. Note that the gain is in   //
// voltage scaling (not dB), so a value of 1.0 (not 0.0) is unity gain.     //
//////////////////////////////////////////////////////////////////////////////

static float calculate_gain (WavpackContext *wpc, int *pSoftClip)
{
    *pSoftClip = FALSE;

    if (config_bits & (REPLAYGAIN_TRACK | REPLAYGAIN_ALBUM)) {
        float gain_value = 0.0, peak_value = 1.0;
        char value [32];

        if ((config_bits & REPLAYGAIN_ALBUM) && WavpackGetTagItem (wpc, "replaygain_album_gain", value, sizeof (value))) {
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

        if (peak_value * gain_value > 1.0) {
            if (config_bits & PREVENT_CLIPPING)
                gain_value = (float)(1.0 / peak_value);
            else if (config_bits & SOFTEN_CLIPPING)
                *pSoftClip = TRUE;
        }

        return gain_value;
    }
    else
        return 1.0;
}

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

// Convert a Unicode UTF-8 format string into its Ansi equivalent. The
// conversion is done in-place so the maximum length of the string buffer must
// be specified because the string may become longer or shorter. If the
// resulting string will not fit in the specified buffer size then it is
// truncated.

static UTF8ToAnsi (char *string, int len)
{
    int max_chars = (int) strlen (string);
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

    free (temp);
}
