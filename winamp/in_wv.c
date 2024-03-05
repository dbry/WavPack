////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 2000 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// in_wv_c.c

// .WV input plug-in for WavPack

#include <windows.h>
#include <fcntl.h>
#include <stdio.h>
#include <mmreg.h>
#include <msacm.h>
#include <math.h>
#include <sys/stat.h>
#include <shlobj.h>
#include <io.h>

#include "in2.h"
#include "wavpack.h"
#include "resource.h"
#include "wasabi/wasabi.h"

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#ifdef __MINGW32__              // mingw32's _ftelli64() and _fseeki64() are defective
#define _ftelli64 ftello64
#define _fseeki64 fseeko64
#endif

static float calculate_gain (WavpackContext *wpc, int *pSoftClip);
static void *decimation_init (int num_channels, int ratio);
static int decimation_run (void *context, int32_t *samples, int num_samples);
static void decimation_reset (void *context);
static void *decimation_destroy (void *context);

#define PLUGIN_VERSION "2.8.0.3"
//#define DEBUG_CONSOLE
#define UNICODE_METADATA

// use CRT. Good. Useful. Portable.
BOOL WINAPI DllMain (HANDLE hInst, ULONG ul_reason_for_call, LPVOID lpReserved)
{
    return TRUE;
}

// post this to the main window at end of file (after playback as stopped)
#define WM_WA_MPEG_EOF WM_USER+2

#define MAX_NCH 8

static struct wpcnxt {
    WavpackContext *wpc;    // WavPack context provided by library
    float play_gain;        // playback gain (for replaygain support)
    int soft_clipping;      // soft clipping active for playback
    int output_bits;        // 16, 24, or 32 bits / sample
    int32_t sample_buffer[576*MAX_NCH*4];  // sample buffer
    float error [MAX_NCH];  // error term for noise shaping
    char lastfn[MAX_PATH];  // filename stored for comparisons only
    wchar_t w_lastfn[MAX_PATH];// w_filename stored for comparisons only
    FILE *wv_id, *wvc_id;   // file pointer when we use reader callbacks
    void *decimation_cnxt;  // context for 4x decimation (DSD playback)
} curr, edit, info;

In_Module mod;          // the output module (declared near the bottom of this file)
int decode_pos_ms;      // current decoding position, in milliseconds
int paused;             // are we paused?
int seek_needed;        // if != -1, it is the point that the decode thread should seek to, in ms.

#define ALLOW_WVC               0x1
#define REPLAYGAIN_TRACK        0x2
#define REPLAYGAIN_ALBUM        0x4
#define SOFTEN_CLIPPING         0x8
#define PREVENT_CLIPPING        0x10

#define ALWAYS_16BIT            0x20    // new flags added for version 2.5
#define ALLOW_MULTICHANNEL      0x40
#define REPLAYGAIN_24BIT        0x80

int config_bits;        // all configuration goes here

int killDecodeThread=0;                         // the kill switch for the decode thread
HANDLE thread_handle=INVALID_HANDLE_VALUE;      // the handle to the decode thread

DWORD WINAPI __stdcall DecodeThread(void *b);   // the decode thread procedure

HMODULE hResources;                             // module handle for resources to use

static BOOL CALLBACK WavPackDlgProc (HWND, UINT, WPARAM, LPARAM);

// static const GUID InWvLangGuid =
// { 0x6de2e465, 0x690e, 0x4df1, { 0xb6, 0xe2, 0x2a, 0x9b, 0x33, 0xed, 0x3d, 0xbb } };
static const char *InWvLangGuid = "{6DE2E465-690E-4df1-B6E2-2A9B33ED3DBB}";

static void configure_resources (void)
{
    char CheckGuid [64];

    if (!hResources) {
        hResources = GetModuleHandle ("in_wv.lng");

        if (hResources)
            if (!LoadString (hResources, IDS_GUID, CheckGuid, sizeof (CheckGuid)) ||
                _stricmp (InWvLangGuid, CheckGuid))
                    hResources = NULL;
    }

    if (!hResources)
        hResources = GetModuleHandle ("in_wv.dll");
}

typedef HRESULT (WINAPI *getfolderpath_t)(HWND,int,HANDLE,DWORD,LPSTR); /* SHGetFolderPathA */
typedef BOOL (WINAPI *getspecialfolderpath_t)(HWND,LPSTR,int,BOOL);     /* SHGetSpecialFolderPathA */

static int get_app_path (char *app_path)
{
    static char file_path [MAX_PATH], tried, result;

    HINSTANCE hinstLib;
    getfolderpath_t getfolderpath;

    if (tried) {
        if (result)
            strcpy (app_path, file_path);

        return result;
    }

    tried = TRUE;
    hinstLib = LoadLibrary ("shell32.dll");

    if (hinstLib) {
        getfolderpath = (getfolderpath_t) GetProcAddress (hinstLib, "SHGetFolderPathA");

        if (getfolderpath && SUCCEEDED (getfolderpath (NULL, CSIDL_APPDATA | 0x8000, NULL, 0, file_path)))
            result = TRUE;

        if (!result) {
            getspecialfolderpath_t getspecialfolderpath;
            getspecialfolderpath = (getspecialfolderpath_t) GetProcAddress (hinstLib, "SHGetSpecialFolderPathA");
            if (getspecialfolderpath && getspecialfolderpath (NULL, file_path, CSIDL_APPDATA, TRUE) != 0)
                result = TRUE;
        }

        FreeLibrary (hinstLib);
    }

    if (!result) {
        hinstLib = LoadLibrary ("shfolder.dll");

        if (hinstLib) {
            getfolderpath = (getfolderpath_t) GetProcAddress (hinstLib, "SHGetFolderPathA");

            if (getfolderpath && SUCCEEDED (getfolderpath (NULL, CSIDL_APPDATA | 0x8000, NULL, 0, file_path)))
                result = TRUE;

            FreeLibrary (hinstLib);
        }
    }

    if (result)
        strcpy (app_path, file_path);

    return result;
}

void debug_write (char *str);

void config (HWND hwndParent)
{
    char dllname [512];
    int temp_config;
    HANDLE confile;
    DWORD result;

    if (!hResources)
        configure_resources ();

    temp_config = (int) DialogBoxParam (hResources, "WinAmp", hwndParent, (DLGPROC) WavPackDlgProc, config_bits);

    if (temp_config == config_bits || (temp_config & 0xffffff00) ||
        (temp_config & 6) == 6 || (temp_config & 0x18) == 0x18)
            return;

    config_bits = temp_config;

    if (get_app_path (dllname)) {
        strcat (dllname, "\\WavPack\\in_wv.dat");

        confile = CreateFile (dllname, GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (confile == INVALID_HANDLE_VALUE) {
            get_app_path (dllname);
            strcat (dllname, "\\WavPack");

            if (CreateDirectory (dllname, NULL)) {
                strcat (dllname, "\\in_wv.dat");

                confile = CreateFile (dllname, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            }
        }

        if (confile == INVALID_HANDLE_VALUE)
            return;

        WriteFile (confile, &config_bits, sizeof (config_bits), &result, NULL);
        CloseHandle (confile);
    }
}

BOOL CALLBACK WavPackDlgProc (HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    char tempstr [128];
    static int local_config;

    switch (message) {
        case WM_INITDIALOG:
            local_config = (int) lParam;

            CheckDlgButton (hDlg, IDC_USEWVC, local_config & ALLOW_WVC);
            CheckDlgButton (hDlg, IDC_ALWAYS_16BIT, local_config & ALWAYS_16BIT);
            CheckDlgButton (hDlg, IDC_MULTICHANNEL, local_config & ALLOW_MULTICHANNEL);

            if (LoadString (hResources, IDS_DISABLED, tempstr, sizeof (tempstr)))
                SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_ADDSTRING, 0, (LPARAM) tempstr);

            if (LoadString (hResources, IDS_USE_TRACK, tempstr, sizeof (tempstr)))
                SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_ADDSTRING, 0, (LPARAM) tempstr);

            if (LoadString (hResources, IDS_USE_ALBUM, tempstr, sizeof (tempstr)))
                SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_ADDSTRING, 0, (LPARAM) tempstr);

            SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_SETCURSEL, (local_config >> 1) & 3, 0);

            if (LoadString (hResources, IDS_JUST_CLIP, tempstr, sizeof (tempstr)))
                SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_ADDSTRING, 0, (LPARAM) tempstr);

            if (LoadString (hResources, IDS_SOFT_CLIP, tempstr, sizeof (tempstr)))
                SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_ADDSTRING, 0, (LPARAM) tempstr);

            if (LoadString (hResources, IDS_PREVENT_CLIP, tempstr, sizeof (tempstr)))
                SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_ADDSTRING, 0, (LPARAM) tempstr);

            SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_SETCURSEL, (local_config >> 3) & 3, 0);

            CheckDlgButton (hDlg, IDC_24BIT_RG, local_config & REPLAYGAIN_24BIT);

            if (!(local_config & (REPLAYGAIN_TRACK | REPLAYGAIN_ALBUM))) {
                EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING_TEXT), FALSE);
                EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING), FALSE);
                EnableWindow (GetDlgItem (hDlg, IDC_24BIT_RG), FALSE);
            }

            SetFocus (GetDlgItem (hDlg, IDC_USEWVC));
            return FALSE;

        case WM_COMMAND:
            switch (LOWORD (wParam)) {
                case IDC_REPLAYGAIN:
                    if (SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_GETCURSEL, 0, 0)) {
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING_TEXT), TRUE);
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING), TRUE);
                        EnableWindow (GetDlgItem (hDlg, IDC_24BIT_RG), TRUE);
                    }
                    else {
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING_TEXT), FALSE);
                        EnableWindow (GetDlgItem (hDlg, IDC_CLIPPING), FALSE);
                        EnableWindow (GetDlgItem (hDlg, IDC_24BIT_RG), FALSE);
                    }

                    break;

                case IDOK:
                    local_config = 0;

                    if (IsDlgButtonChecked (hDlg, IDC_USEWVC))
                        local_config |= ALLOW_WVC;

                    if (IsDlgButtonChecked (hDlg, IDC_ALWAYS_16BIT))
                        local_config |= ALWAYS_16BIT;

                    if (IsDlgButtonChecked (hDlg, IDC_MULTICHANNEL))
                        local_config |= ALLOW_MULTICHANNEL;

                    local_config |= SendDlgItemMessage (hDlg, IDC_REPLAYGAIN, CB_GETCURSEL, 0, 0) << 1;
                    local_config |= SendDlgItemMessage (hDlg, IDC_CLIPPING, CB_GETCURSEL, 0, 0) << 3;

                    if (IsDlgButtonChecked (hDlg, IDC_24BIT_RG))
                        local_config |= REPLAYGAIN_24BIT;

                case IDCANCEL:
                    EndDialog (hDlg, local_config);
                    return TRUE;
            }

            break;
    }

    return FALSE;
}

void about (HWND hwndParent)
{
    char about_title [128], about_string [256], about_format [256];

    if (!hResources)
        configure_resources ();

    if (!LoadString (hResources, IDS_ABOUT, about_title, sizeof (about_title)) ||
        !LoadString (hResources, IDS_FORMAT, about_format, sizeof (about_format)))
            return;

    sprintf (about_string, about_format, PLUGIN_VERSION, 2017);
    MessageBox (hwndParent, about_string, about_title, MB_OK);
}

void init() { /* any one-time initialization goes here (configuration reading, etc) */
    char dllname [512];
    HMODULE module;
    HANDLE confile;
    DWORD result;

    Wasabi_Init ();
    config_bits = ALLOW_WVC | ALLOW_MULTICHANNEL;   // reasonable config defaults

    // first, try to read config from the application-specific data folder

    if (get_app_path (dllname)) {
        strcat (dllname, "\\WavPack\\in_wv.dat");

        confile = CreateFile (dllname, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (confile != INVALID_HANDLE_VALUE) {

            if (ReadFile (confile, &config_bits, sizeof (config_bits), &result, NULL) &&
                result == sizeof (config_bits)) {
                    CloseHandle (confile);
                    return;
                }

            CloseHandle (confile);
        }
    }

    // if nothing there, try our old location (next to the in_wv.dll plugin)...
    // we no longer write here, but trying to read here should avoid configuration loss on upgrade

    module = GetModuleHandle ("in_wv.dll");

    if (module && GetModuleFileName (module, dllname, sizeof (dllname))) {
        dllname [strlen (dllname) - 2] = 'a';
        dllname [strlen (dllname) - 1] = 't';

        confile = CreateFile (dllname, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (confile != INVALID_HANDLE_VALUE) {

            if (ReadFile (confile, &config_bits, sizeof (config_bits), &result, NULL) &&
                result == sizeof (config_bits)) {
                    CloseHandle (confile);
                    return;
                }

            CloseHandle (confile);
        }
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
    Wasabi_Quit ();
}

int isourfile(char *fn)
{
    return 0;
}
// used for detecting URL streams.. unused here. strncmp(fn,"http://",7) to detect HTTP streams, etc

int play (char *fn)
{
    int num_chans, sample_rate;
    char error [128];
    int maxlatency;
    DWORD thread_id;
    int open_flags;

#ifdef DEBUG_CONSOLE
    sprintf (error, "play (%s)\n", fn);
    debug_write (error);
#endif

    open_flags = OPEN_TAGS | OPEN_NORMALIZE | OPEN_DSD_AS_PCM;

    if (config_bits & ALLOW_WVC)
        open_flags |= OPEN_WVC;

    if (!(config_bits & ALLOW_MULTICHANNEL))
        open_flags |= OPEN_2CH_MAX;

    curr.wpc = WavpackOpenFileInput (fn, error, open_flags, 0);

    if (!curr.wpc)           // error opening file, just return error
        return -1;

    num_chans = WavpackGetReducedChannels (curr.wpc);
    curr.output_bits = WavpackGetBitsPerSample (curr.wpc) > 16 ? 24 : 16;

    if (config_bits & ALWAYS_16BIT)
        curr.output_bits = 16;
    else if ((config_bits & (REPLAYGAIN_TRACK | REPLAYGAIN_ALBUM)) &&
        (config_bits & REPLAYGAIN_24BIT))
            curr.output_bits = 24;

    if (num_chans > MAX_NCH) {    // don't allow too many channels!
        WavpackCloseFile (curr.wpc);
        return -1;
    }

    curr.play_gain = calculate_gain (curr.wpc, &curr.soft_clipping);
    strcpy (curr.lastfn, fn);

    paused = 0;
    decode_pos_ms = 0;
    seek_needed = -1;

    sample_rate = WavpackGetSampleRate (curr.wpc);

    if (sample_rate >= 256000) {
        curr.decimation_cnxt = decimation_init (num_chans, 4);
        sample_rate /= 4;
    }

    maxlatency = mod.outMod->Open (sample_rate, num_chans, curr.output_bits, -1, -1);

    if (maxlatency < 0) { // error opening device
        curr.decimation_cnxt = decimation_destroy (curr.decimation_cnxt);
        curr.wpc = WavpackCloseFile (curr.wpc);
        return -1;
    }

    // dividing by 1000 for the first parameter of setinfo makes it
    // display 'H'... for hundred.. i.e. 14H Kbps.

    mod.SetInfo (0, (sample_rate + 500) / 1000, num_chans, 1);

    // initialize vis stuff

    mod.SAVSAInit (maxlatency, sample_rate);
    mod.VSASetInfo (sample_rate, num_chans);

    mod.outMod->SetVolume (-666);       // set the output plug-ins default volume

    killDecodeThread=0;

    thread_handle = (HANDLE) CreateThread (NULL, 0, (LPTHREAD_START_ROUTINE) DecodeThread,
        (void *) &killDecodeThread, 0, &thread_id);

    if (SetThreadPriority (thread_handle, THREAD_PRIORITY_HIGHEST) == 0) {
        curr.decimation_cnxt = decimation_destroy (curr.decimation_cnxt);
        curr.wpc = WavpackCloseFile (curr.wpc);
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

    if (curr.wpc)
        curr.wpc = WavpackCloseFile (curr.wpc);

    curr.decimation_cnxt = decimation_destroy (curr.decimation_cnxt);
    mod.outMod->Close ();
    mod.SAVSADeInit ();
}

int getlength()
{
    return (int)(WavpackGetNumSamples64 (curr.wpc) * 1000.0 / WavpackGetSampleRate (curr.wpc));
}

int getoutputtime()
{
    if (seek_needed == -1)
        return decode_pos_ms + (mod.outMod->GetOutputTime () - mod.outMod->GetWrittenTime ());
    else
        return seek_needed;
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

static void generate_format_string (WavpackContext *wpc, char *string, int maxlen, int wide);
static int UTF8ToWideChar (const unsigned char *pUTF8, unsigned short *pWide);
static int WideCharToUTF8 (const unsigned short *Wide, unsigned char *pUTF8, int len);
static void AnsiToUTF8 (char *string, int len);
static void UTF8ToAnsi (char *string, int len);

int infoDlg (char *fn, HWND hwnd)
{
    char string [2048];
    unsigned short w_string [2048];
    WavpackContext *wpc;
    int open_flags;

    open_flags = OPEN_TAGS | OPEN_NORMALIZE | OPEN_DSD_AS_PCM;

    if (config_bits & ALLOW_WVC)
        open_flags |= OPEN_WVC;

    if (!(config_bits & ALLOW_MULTICHANNEL))
        open_flags |= OPEN_2CH_MAX;

    wpc = WavpackOpenFileInput (fn, string, open_flags, 0);

    if (wpc) {
        int mode = WavpackGetMode (wpc);

        generate_format_string (wpc, string, sizeof (string), 1);

        if (mode & MODE_VALID_TAG) {
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
                if (!(mode & MODE_APETAG))
                     AnsiToUTF8 (value, sizeof (value));

                sprintf (string + strlen (string), "\nTitle:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "artist", value, sizeof (value))) {
                if (!(mode & MODE_APETAG))
                     AnsiToUTF8 (value, sizeof (value));

                sprintf (string + strlen (string), "\nArtist:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "album", value, sizeof (value))) {
                if (!(mode & MODE_APETAG))
                     AnsiToUTF8 (value, sizeof (value));

                sprintf (string + strlen (string), "\nAlbum:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "genre", value, sizeof (value))) {
                if (!(mode & MODE_APETAG))
                     AnsiToUTF8 (value, sizeof (value));

                sprintf (string + strlen (string), "\nGenre:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "comment", value, sizeof (value))) {
                if (!(mode & MODE_APETAG))
                     AnsiToUTF8 (value, sizeof (value));

                sprintf (string + strlen (string), "\nComment:  %s", value);
            }

            if (WavpackGetTagItem (wpc, "year", value, sizeof (value)))
                sprintf (string + strlen (string), "\nYear:  %s", value);

            if (WavpackGetTagItem (wpc, "track", value, sizeof (value)))
                sprintf (string + strlen (string), "\nTrack:  %s", value);

            strcat (string, "\n");
        }

        UTF8ToWideChar (string, w_string);
        MessageBoxW (hwnd, w_string, L"WavPack File Info Box", MB_OK);
        wpc = WavpackCloseFile (wpc);
    }
    else
        MessageBox (hwnd, string, "WavPack Decoder", MB_OK);

    return 0;
}

void getfileinfo (char *filename, char *title, int *length_in_ms)
{
    if (!filename || !*filename) {      // currently playing file

        if (length_in_ms)
            *length_in_ms = getlength ();

        if (title) {
            if (WavpackGetTagItem (curr.wpc, "title", NULL, 0)) {
                char art [128], ttl [128];

                WavpackGetTagItem (curr.wpc, "title", ttl, sizeof (ttl));

                if (WavpackGetMode (curr.wpc) & MODE_APETAG)
                     UTF8ToAnsi (ttl, sizeof (ttl));

                if (WavpackGetTagItem (curr.wpc, "artist", art, sizeof (art))) {
                    if (WavpackGetMode (curr.wpc) & MODE_APETAG)
                        UTF8ToAnsi (art, sizeof (art));

                    sprintf (title, "%s - %s", art, ttl);
                }
                else
                    strcpy (title, ttl);
            }
            else {
                char *p = curr.lastfn + strlen (curr.lastfn);

                while (*p != '\\' && p >= curr.lastfn)
                    p--;

                strcpy(title,++p);
            }
        }
    }
    else {      // some other file
        WavpackContext *wpc;
        char error [128];
        int open_flags;

        if (length_in_ms)
            *length_in_ms = -1000;

        if (title)
            *title = 0;

        open_flags = OPEN_TAGS | OPEN_NORMALIZE | OPEN_DSD_AS_PCM;

        if (config_bits & ALLOW_WVC)
            open_flags |= OPEN_WVC;

        if (!(config_bits & ALLOW_MULTICHANNEL))
            open_flags |= OPEN_2CH_MAX;

        wpc = WavpackOpenFileInput (filename, error, open_flags, 0);

        if (wpc) {
            if (length_in_ms)
                *length_in_ms = (int)(WavpackGetNumSamples64 (wpc) * 1000.0 / WavpackGetSampleRate (wpc));

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

static int read_samples (struct wpcnxt *cnxt, int num_samples);

DWORD WINAPI __stdcall DecodeThread (void *b)
{
    int num_chans, sample_rate;
    int done = 0;

    memset (curr.error, 0, sizeof (curr.error));
    num_chans = WavpackGetReducedChannels (curr.wpc);
    sample_rate = WavpackGetSampleRate (curr.wpc);

    while (!*((int *)b) ) {

        if (seek_needed != -1) {
            int seek_position = seek_needed;

            seek_needed = -1;

            if (seek_position > getlength () - 1000 && getlength () > 1000)
                seek_position = getlength () - 1000; // don't seek to last second

            mod.outMod->Flush (decode_pos_ms = seek_position);
            decimation_reset (curr.decimation_cnxt);

            if (WavpackSeekSample64 (curr.wpc, (int64_t)(sample_rate / 1000.0 * seek_position))) {
                decode_pos_ms = (int)(WavpackGetSampleIndex64 (curr.wpc) * 1000.0 / sample_rate);
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
        else if (mod.outMod->CanWrite() >= ((576 * num_chans * (curr.output_bits / 8)) << (mod.dsp_isactive () ? 1 : 0))) {
            int tsamples = read_samples (&curr, 576) * num_chans;
            int tbytes = tsamples * (curr.output_bits/8);

            if (tsamples) {
                mod.SAAddPCMData ((char *) curr.sample_buffer, num_chans, curr.output_bits, decode_pos_ms);
                mod.VSAAddPCMData ((char *) curr.sample_buffer, num_chans, curr.output_bits, decode_pos_ms);
                decode_pos_ms = (int)(WavpackGetSampleIndex64 (curr.wpc) * 1000.0 / sample_rate);

                if (mod.dsp_isactive())
                    tbytes = mod.dsp_dosamples ((short *) curr.sample_buffer,
                        tsamples / num_chans, curr.output_bits, num_chans, sample_rate) * (num_chans * (curr.output_bits/8));

                mod.outMod->Write ((char *) curr.sample_buffer, tbytes);
            }
            else
                done = 1;
        }
        else {
            mod.SetInfo ((int) ((WavpackGetInstantBitrate (curr.wpc) + 500.0) / 1000.0), -1, -1, 1);
            Sleep(20);
        }
    }

    return 0;
}

/********* These functions provide the "transcoding" mode of winamp. *********/

__declspec (dllexport) intptr_t winampGetExtendedRead_open (
    const char *fn, int *size, int *bps, int *nch, int *srate)
{
    struct wpcnxt *cnxt = (struct wpcnxt *) malloc (sizeof (struct wpcnxt));
    int num_chans, sample_rate, open_flags;
    int64_t actual_size;
    char error [128];

#ifdef DEBUG_CONSOLE
    sprintf (error, "Read_open (%s)\n", fn);
    debug_write (error);
#endif

    if (!cnxt)
        return 0;

    memset (cnxt, 0, sizeof (struct wpcnxt));
    open_flags = OPEN_NORMALIZE | OPEN_WVC | OPEN_DSD_AS_PCM;

    if (!(config_bits & ALLOW_MULTICHANNEL) || *nch == 2)
        open_flags |= OPEN_2CH_MAX;

    if (config_bits & (REPLAYGAIN_TRACK | REPLAYGAIN_ALBUM))
        open_flags |= OPEN_TAGS;

    cnxt->wpc = WavpackOpenFileInput (fn, error, open_flags, 0);

    if (!cnxt->wpc) {           // error opening file, just return error
        free (cnxt);
        return 0;
    }

    num_chans = WavpackGetReducedChannels (cnxt->wpc);
    sample_rate = WavpackGetSampleRate (cnxt->wpc);

    if (num_chans > MAX_NCH) {
        WavpackCloseFile (cnxt->wpc);
        free (cnxt);
        return 0;
    }

    if (*bps != 16 && *bps != 24 && *bps != 32) {
        cnxt->output_bits = WavpackGetBitsPerSample (cnxt->wpc) > 16 ? 24 : 16;

        if (config_bits & ALWAYS_16BIT)
            cnxt->output_bits = 16;
        else if ((config_bits & (REPLAYGAIN_TRACK | REPLAYGAIN_ALBUM)) &&
            (config_bits & REPLAYGAIN_24BIT))
                cnxt->output_bits = 24;
    }
    else
        cnxt->output_bits = *bps;

    if (sample_rate >= 256000) {
        cnxt->decimation_cnxt = decimation_init (num_chans, 4);
        sample_rate /= 4;
    }

    *nch = num_chans;
    *srate = sample_rate;
    *bps = cnxt->output_bits;
    actual_size = WavpackGetNumSamples64 (cnxt->wpc) * (*bps / 8) * (*nch);

    if (cnxt->decimation_cnxt)
        actual_size /= 4;

    if (actual_size <= 2147483647)
        *size = (int) actual_size;
    else
        *size = -1;

    cnxt->play_gain = calculate_gain (cnxt->wpc, &cnxt->soft_clipping);

#ifdef DEBUG_CONSOLE
    sprintf (error, "Read_open success! nch=%d, srate=%d, bps=%d, size=%d\n",
        *nch, *srate, *bps, *size);
    debug_write (error);
#endif

    return (intptr_t) cnxt;
}

__declspec (dllexport) intptr_t winampGetExtendedRead_getData (
    intptr_t handle, char *dest, int len, int *killswitch)
{
    struct wpcnxt *cnxt = (struct wpcnxt *) handle;
    int num_chans = WavpackGetReducedChannels (cnxt->wpc);
    int bytes_per_sample = num_chans * cnxt->output_bits / 8;
    int used = 0;

#ifdef DEBUG_CONSOLE
    char error [128];
#endif

    while (used < len && !*killswitch) {
        int nsamples = (len - used) / bytes_per_sample, tsamples;

        if (!nsamples)
            break;
        else if (nsamples > 576)
            nsamples = 576;

        tsamples = read_samples (cnxt, nsamples) * num_chans;

        if (tsamples) {
            int tbytes = tsamples * (cnxt->output_bits/8);

            memcpy (dest + used, cnxt->sample_buffer, tbytes);
            used += tbytes;
        }
        else
            break;
    }

#ifdef DEBUG_CONSOLE
    sprintf (error, "Read_getData (%d), actually read %d\n", len, used);
    debug_write (error);
#endif

    return used;
}

__declspec (dllexport) int winampGetExtendedRead_setTime (intptr_t handle, int millisecs)
{
    struct wpcnxt *cnxt = (struct wpcnxt *) handle;
    int sample_rate = WavpackGetSampleRate (cnxt->wpc);

    decimation_reset (cnxt->decimation_cnxt);
    return WavpackSeekSample64 (cnxt->wpc, (int64_t)(sample_rate / 1000.0 * millisecs));
}

__declspec (dllexport) void winampGetExtendedRead_close (intptr_t handle)
{
    struct wpcnxt *cnxt = (struct wpcnxt *) handle;

#ifdef DEBUG_CONSOLE
    char error [128];

    sprintf (error, "Read_close ()\n");
    debug_write (error);
#endif

    WavpackCloseFile (cnxt->wpc);
    cnxt->decimation_cnxt = decimation_destroy (cnxt->decimation_cnxt);
    free (cnxt);
}


/* This is a generic function to read WavPack samples and convert them to a
 * form usable by winamp. It includes conversion of any WavPack format
 * (including ieee float) to 16, 24, or 32-bit integers (with noise shaping
 * for the 16-bit case) and replay gain implementation (with optional soft
 * clipping). It is used by both the regular "play" code and the newer
 * transcoding functions.
 *
 * The num_samples parameter is the number of "composite" samples to
 * convert and is limited currently to 576 samples for legacy reasons. The
 * return value is the number of samples actually converted and will be
 * equal to the number requested unless an error occurs or the end-of-file
 * is encountered. The converted samples are stored (interleaved) at
 * cnxt->sample_buffer[].
 */

static int read_samples (struct wpcnxt *cnxt, int num_samples)
{
    int num_chans = WavpackGetReducedChannels (cnxt->wpc), samples, tsamples;

    if (cnxt->decimation_cnxt) {
        samples = WavpackUnpackSamples (cnxt->wpc, cnxt->sample_buffer, num_samples * 4);
        samples = decimation_run (cnxt->decimation_cnxt, cnxt->sample_buffer, samples);
    }
    else
        samples = WavpackUnpackSamples (cnxt->wpc, cnxt->sample_buffer, num_samples);

    tsamples = samples * num_chans;

    if (tsamples) {
        if (!(WavpackGetMode (cnxt->wpc) & MODE_FLOAT)) {
            float scaler = (float) (1.0 / ((unsigned long) 1 << (WavpackGetBytesPerSample (cnxt->wpc) * 8 - 1)));
            float *fptr = (float *) cnxt->sample_buffer;
            int32_t *lptr = cnxt->sample_buffer;
            int cnt = tsamples;

            while (cnt--)
                *fptr++ = *lptr++ * scaler;
        }

        if (cnxt->play_gain != 1.0) {
            float *fptr = (float *) cnxt->sample_buffer;
            int cnt = tsamples;
            double outval;

            while (cnt--) {
                outval = *fptr * cnxt->play_gain;

                if (cnxt->soft_clipping) {
                    if (outval > 0.75)
                        outval = 1.0 - (0.0625 / (outval - 0.5));
                    else if (outval < -0.75)
                        outval = -1.0 - (0.0625 / (outval + 0.5));
                }

                *fptr++ = (float) outval;
            }
        }

        if (cnxt->output_bits == 16) {
            float *fptr = (float *) cnxt->sample_buffer;
            short *sptr = (short *) cnxt->sample_buffer;
            int cnt = samples, ch;

            while (cnt--)
                for (ch = 0; ch < num_chans; ++ch) {
                    int dst;

                    *fptr -= cnxt->error [ch];

                    if (*fptr >= 1.0)
                        dst = 32767;
                    else if (*fptr <= -1.0)
                        dst = -32768;
                    else
                        dst = (int) floor (*fptr * 32768.0);

                    cnxt->error [ch] = (float)(dst / 32768.0 - *fptr++);
                    *sptr++ = dst;
                }
        }
        else if (cnxt->output_bits == 24) {
            unsigned char *cptr = (unsigned char *) cnxt->sample_buffer;
            float *fptr = (float *) cnxt->sample_buffer;
            int cnt = tsamples;
            long outval;

            while (cnt--) {
                if (*fptr >= 1.0)
                    outval = 8388607;
                else if (*fptr <= -1.0)
                    outval = -8388608;
                else
                    outval = (int) floor (*fptr * 8388608.0);

                *cptr++ = (unsigned char) outval;
                *cptr++ = (unsigned char) (outval >> 8);
                *cptr++ = (unsigned char) (outval >> 16);
                fptr++;
            }
        }
        else if (cnxt->output_bits == 32) {
            float *fptr = (float *) cnxt->sample_buffer;
            long *sptr = (long *) cnxt->sample_buffer;
            int cnt = tsamples;

            while (cnt--) {
                if (*fptr >= 1.0)
                    *sptr++ = 8388607 << 8;
                else if (*fptr <= -1.0)
                    *sptr++ = -8388608 << 8;
                else
                    *sptr++ = ((int) floor (*fptr * 8388608.0)) << 8;

                fptr++;
            }
        }
    }

    return samples;
}

static char description [128], file_extensions [128];

In_Module mod =
{
    IN_VER,
    description,
    0,          // hMainWindow
    0,          // hDllInstance
    file_extensions,
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
    char tmp [64], *tmp_ptr = tmp, *fex_ptr = file_extensions;

    if (!hResources)
        configure_resources ();

    if (LoadString (hResources, IDS_DESCRIPTION, tmp, sizeof (tmp)))
        sprintf (description, tmp, PLUGIN_VERSION);
    else
        sprintf (description, "WavPack Decoder %s", PLUGIN_VERSION);

    if (!LoadString (hResources, IDS_FILETYPE, tmp, sizeof (tmp)))
        strcpy (tmp, "WavPack File (*.WV)");

    *fex_ptr++ = 'W';
    *fex_ptr++ = 'V';
    *fex_ptr++ = 0;

    while (*tmp_ptr)
        *fex_ptr++ = *tmp_ptr++;

    *fex_ptr++ = 0;
    *fex_ptr++ = 0;

    return &mod;
}

// This code provides an interface between the reader callback mechanism that
// WavPack uses internally and the standard fstream C library.

static int32_t read_bytes (void *id, void *data, int32_t bcount)
{
    FILE *file = id ? *(FILE**)id : NULL;

    if (file)
        return (int32_t) fread (data, 1, bcount, file);
    else
        return 0;
}

static int64_t get_pos (void *id)
{
    FILE *file = id ? *(FILE**)id : NULL;

    if (file)
        return _ftelli64 (file);
    else
        return -1;
}

static int set_pos_abs (void *id, int64_t pos)
{
    FILE *file = id ? *(FILE**)id : NULL;

    if (file)
        return _fseeki64 (file, pos, SEEK_SET);
    else
        return 0;
}

static int set_pos_rel (void *id, int64_t delta, int mode)
{
    FILE *file = id ? *(FILE**)id : NULL;

    if (file)
        return _fseeki64 (file, delta, mode);
    else
        return -1;
}

static int push_back_byte (void *id, int c)
{
    FILE *file = id ? *(FILE**)id : NULL;

    if (file)
        return ungetc (c, file);
    else
        return EOF;
}

static int64_t get_length (void *id)
{
    FILE *file = id ? *(FILE**)id : NULL;
    LARGE_INTEGER Size;
    HANDLE fHandle;

    if (!file)
        return 0;

    fHandle = (HANDLE)_get_osfhandle(_fileno(file));
    if (fHandle == INVALID_HANDLE_VALUE)
        return 0;

    Size.u.LowPart = GetFileSize(fHandle, (DWORD *) &Size.u.HighPart);

    if (Size.u.LowPart == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        return 0;

    return (int64_t)Size.QuadPart;
}

static int can_seek (void *id)
{
    FILE *file = id ? *(FILE**)id : NULL;
    struct stat statbuf;

    return file && !fstat (_fileno (file), &statbuf) && S_ISREG(statbuf.st_mode);
}

static int32_t write_bytes (void *id, void *data, int32_t bcount)
{
    FILE *file = id ? *(FILE**)id : NULL;

    if (file)
        return (int32_t) fwrite (data, 1, bcount, file);
    else
        return 0;
}

static int truncate_here (void *id)
{
    FILE *file = id ? *(FILE**)id : NULL;

    if (file) {
        #ifdef __WATCOMC__ /*  no _chsize_s() in watcom... */
        HANDLE handle = (HANDLE) _get_osfhandle (_fileno (file));
        return (SetEndOfFile(handle) != 0) ? 0 : -1;
        #else
        int64_t curr_pos = _ftelli64 (file);
        return _chsize_s (_fileno (file), curr_pos);
        #endif
    }
    return 0;
}

static WavpackStreamReader64 freader = {
    read_bytes, write_bytes, get_pos, set_pos_abs, set_pos_rel,
    push_back_byte, get_length, can_seek, truncate_here, NULL
};

/* These functions provide UNICODE support for the winamp media library */

static int metadata_we_can_write (const char *metadata);

static void close_context (struct wpcnxt *cxt)
{
    if (cxt->wpc)
        WavpackCloseFile (cxt->wpc);

    if (cxt->wv_id)
        fclose (cxt->wv_id);

    if (cxt->wvc_id)
        fclose (cxt->wvc_id);

    memset (cxt, 0, sizeof (*cxt));
}

__declspec (dllexport) int winampGetExtendedFileInfo (char *filename, char *metadata, char *ret, int retlen)
{
    int open_flags = OPEN_TAGS | OPEN_DSD_AS_PCM;
    char error [128];
    int retval = 0;

#ifdef DEBUG_CONSOLE
    sprintf (error, "winampGetExtendedFileInfo (%s)\n", metadata);
    debug_write (error);
#endif

    if (!filename || !*filename)
        return retval;

    if (!_stricmp (metadata, "length")) {   /* even if no file, return a 1 and write "0" */
        _snprintf (ret, retlen, "%d", 0);
        retval = 1;
    }

    if (!info.wpc || strcmp (filename, info.lastfn) || !_stricmp (metadata, "formatinformation")) {
        close_context (&info);

        if (!(info.wv_id = fopen (filename, "rb")))
            return retval;

        if (config_bits & ALLOW_WVC) {
            char *wvc_name = malloc (strlen (filename) + 10);

            if (wvc_name) {
                strcpy (wvc_name, filename);
                strcat (wvc_name, "c");
                info.wvc_id = fopen (wvc_name, "rb");
                free (wvc_name);
            }
        }

        info.wpc = WavpackOpenFileInputEx64 (&freader, &info.wv_id,
            info.wvc_id ? &info.wvc_id : NULL, error, open_flags, 0);

        if (!info.wpc) {
            close_context (&info);
            return retval;
        }

        strcpy (info.lastfn, filename);
        info.w_lastfn [0] = 0;
    }

    if (!_stricmp (metadata, "formatinformation")) {
        generate_format_string (info.wpc, ret, retlen, 0);
        retval = 1;
    }
    else if (!_stricmp (metadata, "length")) {
        _snprintf (ret, retlen, "%lld", (long long int)(WavpackGetNumSamples64 (info.wpc) * 1000.0 / WavpackGetSampleRate (info.wpc)));
        retval = 1;
    }
    else if (!_stricmp (metadata, "lossless")) {
        _snprintf (ret, retlen, "%d", (WavpackGetMode (info.wpc) & MODE_LOSSLESS) ? 1 : 0);
        retval = 1;
    }
    else if (!_stricmp (metadata, "numsamples")) {
        _snprintf (ret, retlen, "%lld", (long long int)WavpackGetNumSamples64 (info.wpc));
        retval = 1;
    }
    else if (WavpackGetTagItem (info.wpc, metadata, ret, retlen)) {
        if (WavpackGetMode (info.wpc) & MODE_APETAG)
            UTF8ToAnsi (ret, retlen);

        retval = 1;
    }
    else if (metadata_we_can_write (metadata)) {
        if (retlen)
            *ret = 0;

        retval = 1;
    }

    // This is a little ugly, but since the WavPack library has read the tags off the
    // files, we can close the files (but not the WavPack context) now so that we don't
    // leave handles open. We may access the file again for the "formatinformation"
    // field, so we reopen the file if we get that one.

    if (info.wv_id) {
        fclose (info.wv_id);
        info.wv_id = NULL;
    }

    if (info.wvc_id) {
        fclose (info.wvc_id);
        info.wvc_id = NULL;
    }

    return retval;
}

#ifdef UNICODE_METADATA

__declspec (dllexport) int winampGetExtendedFileInfoW (wchar_t *filename, char *metadata, wchar_t *ret, int retlen)
{
    char error [128], res [256];
    unsigned short w_res [256];
    int open_flags = OPEN_TAGS | OPEN_DSD_AS_PCM;
    int retval = 0;

    retlen >>= 1;       // could this be in bytes, not wchars??

#ifdef DEBUG_CONSOLE
    sprintf (error, "winampGetExtendedFileInfoW (%s)\n", metadata);
    debug_write (error);
#endif

    if (!filename || !*filename)
        return retval;

    if (!_stricmp (metadata, "length")) {   /* even if no file, return a 1 and write "0" */
        _snwprintf (ret, retlen, L"%d", 0);
        retval = 1;
    }

    if (!info.wpc || wcscmp (filename, info.w_lastfn) || !_stricmp (metadata, "formatinformation")) {
        close_context (&info);

        if (!(info.wv_id = _wfopen (filename, L"rb")))
            return retval;

        if (config_bits & ALLOW_WVC) {
            wchar_t *wvc_name = malloc (wcslen (filename) * 2 + 10);

            if (wvc_name) {
                wcscpy (wvc_name, filename);
                wcscat (wvc_name, L"c");
                info.wvc_id = _wfopen (wvc_name, L"rb");
                free (wvc_name);
            }
        }

        info.wpc = WavpackOpenFileInputEx64 (&freader, &info.wv_id,
            info.wvc_id ? &info.wvc_id : NULL, error, open_flags, 0);

        if (!info.wpc) {
            close_context (&info);
            return retval;
        }

        wcscpy (info.w_lastfn, filename);
        info.lastfn [0] = 0;
    }

    if (!_stricmp (metadata, "formatinformation")) {
        char *temp = malloc (retlen), *tp = temp;

        if (temp) {
            generate_format_string (info.wpc, temp, retlen, 0);

            if (!MultiByteToWideChar (CP_THREAD_ACP, MB_PRECOMPOSED, temp, -1, ret, retlen) &&
                !MultiByteToWideChar (CP_ACP, MB_PRECOMPOSED, temp, -1, ret, retlen)) {
                    while (*tp)
                        *ret++ = *tp++;

                    *ret = 0;
            }

            retval = 1;
            free (temp);
        }
    }
    else if (!_stricmp (metadata, "length")) {
        _snwprintf (ret, retlen, L"%lld", (long long int)(WavpackGetNumSamples64 (info.wpc) * 1000.0 / WavpackGetSampleRate (info.wpc)));
        retval = 1;
    }
    else if (!_stricmp (metadata, "lossless")) {
        _snwprintf (ret, retlen, L"%d", (WavpackGetMode (info.wpc) & MODE_LOSSLESS) ? 1 : 0);
        retval = 1;
    }
    else if (!_stricmp (metadata, "numsamples")) {
        _snwprintf (ret, retlen, L"%lld", WavpackGetNumSamples64 (info.wpc));
        retval = 1;
    }
    else if (WavpackGetTagItem (info.wpc, metadata, res, sizeof (res))) {
        if (!(WavpackGetMode (info.wpc) & MODE_APETAG))
            AnsiToUTF8 (res, sizeof (res));

        UTF8ToWideChar (res, w_res);
        wcsncpy (ret, w_res, retlen);
        retval = 1;
    }
    else if (metadata_we_can_write (metadata)) {
        if (retlen)
            *ret = 0;

        retval = 1;
    }

    // This is a little ugly, but since the WavPack library has read the tags off the
    // files, we can close the files (but not the WavPack context) now so that we don't
    // leave handles open. We may access the file again for the "formatinformation"
    // field, so we reopen the file if we get that one.

    if (info.wv_id) {
        fclose (info.wv_id);
        info.wv_id = NULL;
    }

    if (info.wvc_id) {
        fclose (info.wvc_id);
        info.wvc_id = NULL;
    }

    return retval;
}

#endif

int __declspec (dllexport) winampSetExtendedFileInfo (
    const char *filename, const char *metadata, char *val)
{
    char error [128];

#ifdef DEBUG_CONSOLE
    sprintf (error, "winampSetExtendedFileInfo (%s=%s)\n", metadata, val);
    debug_write (error);
#endif

    if (!filename || !*filename || !metadata_we_can_write (metadata))
        return 0;

    if (!edit.wpc || strcmp (filename, edit.lastfn)) {
        if (edit.wpc)
            WavpackCloseFile (edit.wpc);

        edit.wpc = WavpackOpenFileInput (filename, error, OPEN_TAGS | OPEN_EDIT_TAGS | OPEN_DSD_AS_PCM, 0);

        if (!edit.wpc)
            return 0;

        strcpy (edit.lastfn, filename);
        edit.w_lastfn [0] = 0;
    }

    if (strlen (val))
        return WavpackAppendTagItem (edit.wpc, metadata, val, strlen (val));
    else
        return WavpackDeleteTagItem (edit.wpc, metadata);
}

#ifdef UNICODE_METADATA

int __declspec (dllexport) winampSetExtendedFileInfoW (
    const wchar_t *filename, const char *metadata, wchar_t *val)
{
    char error [128], utf8_val [256];

    WideCharToUTF8 (val, utf8_val, sizeof (utf8_val) - 1);

#ifdef DEBUG_CONSOLE
    sprintf (error, "winampSetExtendedFileInfoW (%s=%s)\n", metadata, utf8_val);
    debug_write (error);
#endif

    if (!filename || !*filename || !metadata_we_can_write (metadata))
        return 0;

    if (!edit.wpc || wcscmp (filename, edit.w_lastfn)) {
        if (edit.wpc) {
            WavpackCloseFile (edit.wpc);
            edit.wpc = NULL;
        }

        if (edit.wv_id)
            fclose (edit.wv_id);

        if (!(edit.wv_id = _wfopen (filename, L"r+b")))
            return 0;

        edit.wpc = WavpackOpenFileInputEx64 (&freader, &edit.wv_id, NULL, error, OPEN_TAGS | OPEN_EDIT_TAGS | OPEN_DSD_AS_PCM, 0);

        if (!edit.wpc) {
            fclose (edit.wv_id);
            return 0;
        }

        wcscpy (edit.w_lastfn, filename);
        edit.lastfn [0] = 0;
    }

    if (strlen (utf8_val))
        return WavpackAppendTagItem (edit.wpc, metadata, utf8_val, strlen (utf8_val));
    else
        return WavpackDeleteTagItem (edit.wpc, metadata);
}

#endif

int __declspec (dllexport) winampWriteExtendedFileInfo (void)
{
#ifdef DEBUG_CONSOLE
    debug_write ("winampWriteExtendedFileInfo ()\n");
#endif

    if (edit.wpc) {
        WavpackWriteTag (edit.wpc);
        WavpackCloseFile (edit.wpc);
        edit.wpc = NULL;
    }

    if (edit.wv_id) {
        fclose (edit.wv_id);
        edit.wv_id = NULL;
    }

    close_context (&info);      // make sure we re-read info on any open files
    return 1;
}

// return 1 if you want winamp to show it's own file info dialogue, 0 if you want to show your own (via In_Module.InfoBox)
// if returning 1, remember to implement winampGetExtendedFileInfo("formatinformation")!
__declspec(dllexport) int winampUseUnifiedFileInfoDlg(const wchar_t * fn)
{
    return 1;
}

static const char *writable_metadata [] = {
    "track", "genre", "year", "comment", "artist", "album", "title", "albumartist",
    "composer", "publisher", "disc", "tool", "encoder", "bpm", "category",
    "replaygain_track_gain", "replaygain_track_peak",
    "replaygain_album_gain", "replaygain_album_peak"
};

#define NUM_KNOWN_METADATA (sizeof (writable_metadata) / sizeof (writable_metadata [0]))

static int metadata_we_can_write (const char *metadata)
{
    int i;

    if (!metadata || !*metadata)
        return 0;

    for (i = 0; i < NUM_KNOWN_METADATA; ++i)
        if (!_stricmp (metadata, writable_metadata [i]))
            return 1;

    return 0;
}

static void generate_format_string (WavpackContext *wpc, char *string, int maxlen, int wide)
{
    int qmode = WavpackGetQualifyMode (wpc);
    int mode = WavpackGetMode (wpc);
    char str_floats [32] = "floats";
    char str_ints [32] = "ints";
    char str_hybrid [32] = "hybrid";
    char str_lossless [32] = "lossless";
    char str_lossy [32] = "lossy";
    char str_fast [32] = ", fast";
    char str_high [32] = ", high";
    char str_vhigh [32] = ", v.high";
    char str_extra [32] = ", extra";
    char str_modes [32] = "Modes";
    char str_bitrate [32] = "Average bitrate";
    char str_ratio [32] = "Overall ratio";
    char str_kbps [32] = "kbps";
    char str_md5 [32] = "Original md5";
    unsigned char md5_sum [16];
    char modes [256];
    char fmt [256];

    if (!hResources)
        configure_resources ();

    LoadString (hResources, IDS_FLOATS, str_floats, sizeof (str_floats));
    LoadString (hResources, IDS_INTS, str_ints, sizeof (str_ints));
    LoadString (hResources, IDS_HYBRID, str_hybrid, sizeof (str_hybrid));
    LoadString (hResources, IDS_LOSSLESS, str_lossless, sizeof (str_lossless));
    LoadString (hResources, IDS_LOSSY, str_lossy, sizeof (str_lossy));
    LoadString (hResources, IDS_FAST, str_fast, sizeof (str_fast));
    LoadString (hResources, IDS_HIGH, str_high, sizeof (str_high));
    LoadString (hResources, IDS_VHIGH, str_vhigh, sizeof (str_vhigh));
    LoadString (hResources, IDS_EXTRA, str_extra, sizeof (str_extra));
    LoadString (hResources, IDS_MODES, str_modes, sizeof (str_modes));
    LoadString (hResources, IDS_BITRATE, str_bitrate, sizeof (str_bitrate));
    LoadString (hResources, IDS_RATIO, str_ratio, sizeof (str_ratio));
    LoadString (hResources, IDS_KBPS, str_kbps, sizeof (str_kbps));
    LoadString (hResources, IDS_MD5, str_md5, sizeof (str_md5));

    if (LoadString (hResources, IDS_ENCODER_VERSION, fmt, sizeof (fmt))) {
        _snprintf (string, maxlen, fmt, WavpackGetVersion (wpc));
        while (*string && string++ && maxlen--);
    }

    if (LoadString (hResources, IDS_SOURCE, fmt, sizeof (fmt))) {
        _snprintf (string, maxlen, fmt, (qmode & QMODE_DSD_AUDIO) ? 1 : WavpackGetBitsPerSample (wpc),
            (mode & MODE_FLOAT) ? str_floats : str_ints, WavpackGetNativeSampleRate (wpc));

        while (*string && string++ && maxlen--);
    }

    if (WavpackGetNumChannels (wpc) > 2) {
        if (LoadString (hResources, IDS_MULTICHANNEL, fmt, sizeof (fmt))) {
            _snprintf (string, maxlen, fmt, WavpackGetNumChannels (wpc));
            while (*string && string++ && maxlen--);
        }
    }
    else if (WavpackGetNumChannels (wpc) == 2) {
        if (LoadString (hResources, IDS_STEREO, fmt, sizeof (fmt))) {
            _snprintf (string, maxlen, fmt);
            while (*string && string++ && maxlen--);
        }
    }
    else
        if (LoadString (hResources, IDS_MONO, fmt, sizeof (fmt))) {
            _snprintf (string, maxlen, fmt);
            while (*string && string++ && maxlen--);
        }

    modes [0] = 0;

    if (mode & MODE_HYBRID) {
        strcat (modes, str_hybrid);
        strcat (modes, " ");
    }

    strcat (modes, (mode & MODE_LOSSLESS) ? str_lossless : str_lossy);

    if (mode & MODE_FAST)
        strcat (modes, str_fast);
    else if (mode & MODE_VERY_HIGH)
        strcat (modes, str_vhigh);
    else if (mode & MODE_HIGH)
        strcat (modes, str_high);

    if (mode & MODE_EXTRA)
        strcat (modes, str_extra);

    _snprintf (string, maxlen, "%s:%s  %s\n", str_modes, (wide || strlen (modes) < 24) ? "" : "\n", modes);
    while (*string && string++ && maxlen--);

    if (WavpackGetRatio (wpc) != 0.0) {
        _snprintf (string, maxlen, "%s:  %d %s \n", str_bitrate,
            (int) ((WavpackGetAverageBitrate (wpc, TRUE) + 500.0) / 1000.0), str_kbps);
        while (*string && string++ && maxlen--);
        _snprintf (string, maxlen, "%s:  %.2f : 1 \n", str_ratio, 1.0 / WavpackGetRatio (wpc));
        while (*string && string++ && maxlen--);
    }

    if (WavpackGetMD5Sum (wpc, md5_sum)) {
        char md5s1 [17], md5s2 [17];
        int i;

        for (i = 0; i < 8; ++i) {
            sprintf (md5s1 + i * 2, "%02x", md5_sum [i]);
            sprintf (md5s2 + i * 2, "%02x", md5_sum [i+8]);
        }

        if (wide)
            _snprintf (string, maxlen, "%s:  %s%s\n", str_md5, md5s1, md5s2);
        else
            _snprintf (string, maxlen, "%s:\n  %s\n  %s\n", str_md5, md5s1, md5s2);
    }
}

///////////////////// native "C" functions required for AlbumArt support ///////////////////////////

#ifdef DEBUG_CONSOLE

static char temp_buff [256];

static char *wide2char (const wchar_t *src)
{
    char *dst = temp_buff;

    while (*src)
        *dst++ = (char) *src++;

    *dst = 0;
    return temp_buff;
}

#endif

int WavPack_HandlesExtension(const wchar_t *extension)
{
    return !_wcsicmp (extension, L"wv");
}

int WavPack_GetAlbumArt(const wchar_t *filename, const wchar_t *type, void **bits, size_t *len, wchar_t **mime_type)
{
    unsigned char *buffer;
    char error [128];
    int tag_size, i;
    int retval = 1;

#ifdef DEBUG_CONSOLE
    sprintf (error, "WavPack_GetAlbumArt (%s)\n", wide2char (type));
    debug_write (error);
#endif

    if (!filename || !*filename || _wcsicmp (type, L"cover"))
        return retval;

    if (!info.wpc || wcscmp (filename, info.w_lastfn)) {
        close_context (&info);

        if (!(info.wv_id = _wfopen (filename, L"rb")))
            return retval;

        if (config_bits & ALLOW_WVC) {
            wchar_t *wvc_name = malloc (wcslen (filename) * 2 + 10);

            if (wvc_name) {
                wcscpy (wvc_name, filename);
                wcscat (wvc_name, L"c");
                info.wvc_id = _wfopen (wvc_name, L"rb");
                free (wvc_name);
            }
        }

        info.wpc = WavpackOpenFileInputEx64 (&freader, &info.wv_id,
            info.wvc_id ? &info.wvc_id : NULL, error, OPEN_TAGS | OPEN_DSD_AS_PCM, 0);

        if (!info.wpc) {
            close_context (&info);
            return retval;
        }

        wcscpy (info.w_lastfn, filename);
        info.lastfn [0] = 0;
    }

    tag_size = WavpackGetBinaryTagItem (info.wpc, "Cover Art (Front)", NULL, 0);

    if (!tag_size)
        return retval;

    buffer = Wasabi_Malloc (tag_size);
    WavpackGetBinaryTagItem (info.wpc, "Cover Art (Front)", buffer, tag_size);

    for (i = 0; i < tag_size - 1; ++i)
        if (!buffer [i] && strrchr (buffer, '.')) {
            char *ext = strrchr (buffer, '.') + 1;
            wchar_t *wcptr;

            wcptr = *mime_type = Wasabi_Malloc (strlen (ext) * 2 + 2);

            while (*ext)
                *wcptr++ = *ext++;

            *wcptr = 0;
            *bits = buffer;
            *len = tag_size - i - 1;
            memmove (buffer, buffer + i + 1, *len);
            retval = 0;
#ifdef DEBUG_CONSOLE
            sprintf (error, "WavPack_GetAlbumArt (\"%s\", %lu) success!\n", wide2char (*mime_type), (unsigned long) *len);
            debug_write (error);
#endif
        }

    if (retval)
        Wasabi_Free (buffer);

    // This is a little ugly, but since the WavPack library has read the tags off the
    // files, we can close the files (but not the WavPack context) now so that we don't
    // leave handles open. We may access the file again for the "formatinformation"
    // field, so we reopen the file if we get that one.

    if (info.wv_id) {
        fclose (info.wv_id);
        info.wv_id = NULL;
    }

    if (info.wvc_id) {
        fclose (info.wvc_id);
        info.wvc_id = NULL;
    }

    return retval;
}

int WavPack_SetAlbumArt(const wchar_t *filename, const wchar_t *type, void *bits, size_t len, const wchar_t *mime_type)
{
#if 1
    return 2;   // return 2 to indicate "read-only" cover art for now
#else
    char error [128], name [50], *cp;
    int tag_size, retval = 0;
    unsigned char *buffer;

#ifdef DEBUG_CONSOLE
    sprintf (error, "WavPack_SetAlbumArt (%s)\n", wide2char (mime_type));
    debug_write (error);
#endif

    if (!filename || !*filename || _wcsicmp (type, L"cover") || wcslen (mime_type) > 16)
        return 1;

    strcpy (name, "Cover Art (Front)");
    cp = name + strlen (name);
    *cp++ = '.';

    while (*mime_type)
        *cp++ = (char) *mime_type++;

    *cp = 0;
    tag_size = strlen (name) + 1 + len;
    buffer = malloc (tag_size);
    strcpy (buffer, name);
    memcpy (buffer + strlen (buffer) + 1, bits, len);

    if (!edit.wpc || wcscmp (filename, edit.w_lastfn)) {
        if (edit.wpc) {
            WavpackCloseFile (edit.wpc);
            edit.wpc = NULL;
        }

        if (edit.wv_id)
            fclose (edit.wv_id);

        if (!(edit.wv_id = _wfopen (filename, L"r+b"))) {
            free (buffer);
            return 1;
        }

        edit.wpc = WavpackOpenFileInputEx64 (&freader, &edit.wv_id, NULL, error, OPEN_TAGS | OPEN_EDIT_TAGS | OPEN_DSD_AS_PCM, 0);

        if (!edit.wpc) {
            fclose (edit.wv_id);
            free (buffer);
            return 1;
        }

        wcscpy (edit.w_lastfn, filename);
        edit.lastfn [0] = 0;
    }

    retval = WavpackAppendTagItem (edit.wpc, "Cover Art (Front)", buffer, tag_size);
    free (buffer);

    if (retval) {
        winampWriteExtendedFileInfo ();
        return 0;
    }
    else {
        close_context (&edit);
        return 1;
    }
#endif
}

int WavPack_DeleteAlbumArt(const wchar_t *filename, const wchar_t *type)
{
#if 1
    return 2;   // return 2 to indicate "read-only" cover art for now
#else
    char error [128];

#ifdef DEBUG_CONSOLE
    sprintf (error, "WavPack_DeleteAlbumArt ()\n");
    debug_write (error);
#endif

    if (!filename || !*filename || _wcsicmp (type, L"cover"))
        return 0;

    if (!edit.wpc || wcscmp (filename, edit.w_lastfn)) {
        if (edit.wpc) {
            WavpackCloseFile (edit.wpc);
            edit.wpc = NULL;
        }

        if (edit.wv_id)
            fclose (edit.wv_id);

        if (!(edit.wv_id = _wfopen (filename, L"r+b")))
            return 1;

        edit.wpc = WavpackOpenFileInputEx64 (&freader, &edit.wv_id, NULL, error, OPEN_TAGS | OPEN_EDIT_TAGS | OPEN_DSD_AS_PCM, 0);

        if (!edit.wpc) {
            fclose (edit.wv_id);
            return 1;
        }

        wcscpy (edit.w_lastfn, filename);
        edit.lastfn [0] = 0;
    }

    if (WavpackDeleteTagItem (edit.wpc, "Cover Art (Front)")) {
        winampWriteExtendedFileInfo ();
        return 0;
    }
    else {
        close_context (&edit);
        return 1;
    }
#endif
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

// Convert the Unicode wide-format string into a UTF-8 string using no more
// than the specified buffer length. The wide-format string must be NULL
// terminated and the resulting string will be NULL terminated. The actual
// number of characters converted (not counting terminator) is returned, which
// may be less than the number of characters in the wide string if the buffer
// length is exceeded.

static int WideCharToUTF8 (const unsigned short *Wide, unsigned char *pUTF8, int len)
{
    const unsigned short *pWide = Wide;
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

// Convert a Ansi string into its Unicode UTF-8 format equivalent. The
// conversion is done in-place so the maximum length of the string buffer must
// be specified because the string may become longer or shorter. If the
// resulting string will not fit in the specified buffer size then it is
// truncated.

static void AnsiToUTF8 (char *string, int len)
{
    int max_chars = (int) strlen (string);
    unsigned short *temp = (unsigned short *) malloc ((max_chars + 1) * 2);

    MultiByteToWideChar (CP_ACP, 0, string, -1, temp, max_chars + 1);
    WideCharToUTF8 (temp, (unsigned char *) string, len);
    free (temp);
}

// Convert a Unicode UTF-8 format string into its Ansi equivalent. The
// conversion is done in-place so the maximum length of the string buffer must
// be specified because the string may become longer or shorter. If the
// resulting string will not fit in the specified buffer size then it is
// truncated.

static void UTF8ToAnsi (char *string, int len)
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

/////////////////////////////////////////////////////////////////////////////////////////
// Decimation code for playing DSD (which comes from the library already decimated 8x) //
/////////////////////////////////////////////////////////////////////////////////////////

// sinc low-pass filter, cutoff = fs/12, 80 terms

static int32_t filter [] = {
    50, 464, 968, 711, -1203, -5028, -9818, -13376,
    -12870, -6021, 7526, 25238, 41688, 49778, 43050, 18447,
    -21428, -67553, -105876, -120890, -100640, -41752, 47201, 145510,
    224022, 252377, 208224, 86014, -97312, -301919, -470919, -541796,
    -461126, -199113, 239795, 813326, 1446343, 2043793, 2509064, 2763659,
    2763659, 2509064, 2043793, 1446343, 813326, 239795, -199113, -461126,
    -541796, -470919, -301919, -97312, 86014, 208224, 252377, 224022,
    145510, 47201, -41752, -100640, -120890, -105876, -67553, -21428,
    18447, 43050, 49778, 41688, 25238, 7526, -6021, -12870,
    -13376, -9818, -5028, -1203, 711, 968, 464, 50
};

#define NUM_TERMS ((int)(sizeof (filter) / sizeof (filter [0])))

typedef struct chan_state {
    int delay [NUM_TERMS], index, num_channels, ratio;
} ChanState;

static void *decimation_init (int num_channels, int ratio)
{
    ChanState *sp = malloc (sizeof (ChanState) * num_channels);
    int i;

    if (sp) {
        memset (sp, 0, sizeof (ChanState) * num_channels);

        for (i = 0; i < num_channels; ++i) {
            sp [i].num_channels = num_channels;
            sp [i].index = NUM_TERMS - ratio;
            sp [i].ratio = ratio;
        }
    }

    return sp;
}

static int decimation_run (void *context, int32_t *samples, int num_samples)
{
    int32_t *in_samples = samples, *out_samples = samples;
    ChanState *sp = (ChanState *) context;
    int num_channels, ratio, chan;

    if (!sp)
        return 0;

    num_channels = sp->num_channels;
    ratio = sp->ratio;
    chan = 0;

    while (num_samples) {
        sp = ((ChanState *) context) + chan;

        sp->delay [sp->index++] = *in_samples++;

        if (sp->index == NUM_TERMS) {
            int64_t sum = 0;
            int i;

            for (i = 0; i < NUM_TERMS; ++i)
                sum += (int64_t) filter [i] * sp->delay [i];

            *out_samples++ = (int32_t)(sum >> 24);
            memmove (sp->delay, sp->delay + ratio, sizeof (sp->delay [0]) * (NUM_TERMS - ratio));
            sp->index = NUM_TERMS - ratio;
        }

        if (++chan == num_channels) {
            num_samples--;
            chan = 0;
        }
    }

    return (int)(out_samples - samples) / num_channels;
}

static void decimation_reset (void *context)
{
    ChanState *sp = (ChanState *) context;
    int num_channels, ratio, i;

    if (sp) {
        num_channels = sp->num_channels;
        ratio = sp->ratio;

        memset (sp, 0, sizeof (ChanState) * num_channels);

        for (i = 0; i < num_channels; ++i) {
            sp [i].num_channels = num_channels;
            sp [i].index = NUM_TERMS - ratio;
            sp [i].ratio = ratio;
        }
    }
}

static void *decimation_destroy (void *context)
{
    if (context)
        free (context);

    return NULL;
}

