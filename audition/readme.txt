*****************************************************************
 Adobe Audition File Filter for WavPack  Version 2.14  2016-03-28
 Copyright (c) 2016 David Bryant.  All Rights Reserved.
*****************************************************************

                   ---- INTRODUCTION ----

This plugin allows you to load and save files in the WavPack format.
It will work with any version of Cool Edit and with Adobe Audition
versions from 1.0 to 3.0. For Audition 4.0 and greater or any of the
CS or CC versions, this plugin will not work, but compatible plugins
are available on the WavPack website.

It supports both pure lossless and hybrid WavPack files (with
correction files) and bitdepths of 8, 16, 20, 24, and 32 bits (floating
point). 32-bit float data can be stored natively, or converted in the
filter to 20 or 24-bit integer with selectable noise shaping and
dithering. Also, extra information like cue and play lists,
artist/title information, EBU extensions and even bitmaps can be stored
and retrieved in WavPack files.

All WavPack files can be loaded with the filter except "raw" files
created by WavPack versions prior to 4.0 and any "correction" files
are always automatically used if found.


                   ---- INSTALLATION ----

To install the filter, simply copy (or extract) the file "cool_wv4.flt"
into the standard directory that contains the executable for Audition
or Cool Edit. Note that if you were using the previous version of the
WavPack filter you can leave it usable initially because it has a
different name from the new version. However, once you verify that the
new version is working for you, it is recommended that the old version
be deleted to avoid confusion.


                  ---- DIALOG OPTIONS ----

When you save WavPack files you can use the dialog box for
configuration. The first thing you select is the Compression Mode:

 o Lossless  -> This is the default mode and provides a decent
             compromise between compression ratio and speed.

 o Lossless  -> This mode provides the better compression
    (high)   in WavPack, but is somewhat slower than the default
             mode both in saving and loading files.

 o Lossless  -> This mode provides the highest possible compression
   (v-high)  in WavPack, but is significantly slower than the default
             mode both in saving and loading files and is NOT
             recommended for file to be played on hardware devices.

 o Lossless  -> This mode provides faster operation with somewhat
    (fast)   less compression than the other modes.


 o Hybrid    -> This enables the "hybrid" mode of WavPack (which
             can be either lossless or lossy depending on the
             creation of the "correction" file).

 o Hybrid    -> This is the high quality version of the hybrid mode
   (high)    which provides higher quality lossy files and somewhat
             smaller correction files, but at some cost in speed.

 o Hybrid    -> This is the highest quality version of the hybrid mode
  (v-high)   which provides higher quality lossy files and somewhat
             smaller correction files, but at significant cost in speed.
             This mode is NOT recommended for file to be played on
             hardware devices.

 o Hybrid    -> This mode provides faster operation that the other
   (fast)    hybrid modes, but at some cost in compression ratio and
             lossy audio quality.


If hybrid mode is selected, then the Hybrid Specific portion of the
dialog box is enabled:

 o Bitrate   -> This is where you select the target bitrate of the
   in kbps   lossy file; the values range from the mimimum possible
             for the current file up to a reasonable maximum using
             standard bitrates (although you may type in a custom
             value if you like).

 o Create    -> This option enables the creation of the "correction"
 Correction  file (extension .wvc) that stores the information that
   File      is discarded in creating the lossy file and may be used
             later for lossless operation.


When you save from a 32-bit float file, you will additionally get
these options:

 o 32-bit    -> This causes the filter to store 32-bit float data
   floats    as is. If one of the lossless modes are selected (or
  (type 1)   correction files are selected) then this will store
             the audio with no changes whatsoever. Note that this
             stores the data in Audition's native format range which
             is not compatible with Windows standard wav files. This
             means that if the resulting files are unpacked with the
             command-line unpacker back to wav files they will not
             be playable with normal Windows players (however they
             will be usable by Audition). The WavPack files will be
             playable in all players compatible with WavPack.

 o Normalize -> This forces the filter to normalize the audio to the
   (type 3)  standard wav range (+/- 1.0) so that if a resulting file
             is unpacked by the command-line WavPack unpacker it will
             be fully compatible with standard Windows players. This
             does not affect the file's usability in either Audition
             or other WavPack compatible programs.

 o Convert   -> This causes the filter to create a 20-bit WavPack
  to 20-bit  file. This is probably plenty of resolution for most
     int     applications because is provides a S/N ratio of about
             120 dB and results in significantly smaller files than
             storing all 24 bits.

 o Store as  -> This causes the filter to create a 24-bit WavPack
 24-bit int  file. If the original source of the file was 24 bits
             (such as from a 24-bit DAC or DVD rip) and has not been
             "modified" and the dithering option below is NOT set,
             then this will result in a "lossless" storage operation.
             If modifications have been performed on the raw data
             (like EQ, mixing, etc.) then the data will probably be
             slightly changed during the save, however this added
             noise will be about 144 dB below full scale. Also, if
             the data exceeds the normal +/-32K range, these samples
             will be clipped.

o Noise      -> This option moves the noise generated by quantization
  Shaping    up in frequency, making is less audible and providing
             more dynamic range in the midrange. In most situations
             this alone is sufficient for quality conversions and
             has the advantage that it will NOT alter the audio if
             it already fits losslessly into the specified bitdepth.

o Dithering  -> Add low level dither noise to the signal before
             conversion. This can be useful in avoiding artifacts
             when storing very low level audio, but it has the
             disadvantages that it raises the noise floor slightly
             and alters the audio every save/load cycle.


Finally, this selection is available in all modes:

O Extra      -> This slider selects the amount of extra processing used
processing   to improve the compression ratio and, in hybrid mode, the
             quality of the resulting lossy file. This is equivalent to
             the -x mode of the command-line encoder. When the slider is
             all the way to the left (0) no extra processing is done and
             the fastest possible operation is performed. The rightmost
             position (6) causes an exhaustive search for the best
             compression parameters and is very slow. In some cases this
             extra processing can significantly improve the compression
             ratio, especially for "non-standard" files like those
             containing sythesized signals or those at non-standard
             sampling rates.


WavPack and this plugin are free programs; feel free to give them to
anyone who may find them useful. There is no warrantee provided and you
agree to use them completely at your own risk. If you have any questions
or problems please let me know at david@wavpack.com and be sure to visit
www.wavpack.com for the latest versions of WavPack and this plugin.
