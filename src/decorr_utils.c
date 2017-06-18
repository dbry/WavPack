////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decorr_utils.c

// This module contains the functions that process metadata blocks that are
// specific to the decorrelator. These would be called any time a WavPack
// block was parsed. These are in a module separate from the actual unpack
// decorrelation code (unpack.c) so that if an application just wants to get
// information from WavPack files (rather than actually decoding audio) then
// less code needs to be linked.

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"
#include "decorr_tables.h"      // contains data, only include from this module!

///////////////////////////// executable code ////////////////////////////////

// Read decorrelation terms from specified metadata block into the
// decorr_passes array. The terms range from -3 to 8, plus 17 & 18;
// other values are reserved and generate errors for now. The delta
// ranges from 0 to 7 with all values valid. Note that the terms are
// stored in the opposite order in the decorr_passes array compared
// to packing.

int read_decorr_terms (WavpackStream *wps, WavpackMetadata *wpmd)
{
    int termcnt = wpmd->byte_length;
    unsigned char *byteptr = wpmd->data;
    struct decorr_pass *dpp;

    if (termcnt > MAX_NTERMS)
        return FALSE;

    wps->num_terms = termcnt;

    for (dpp = wps->decorr_passes + termcnt - 1; termcnt--; dpp--) {
        dpp->term = (int)(*byteptr & 0x1f) - 5;
        dpp->delta = (*byteptr++ >> 5) & 0x7;

        if (!dpp->term || dpp->term < -3 || (dpp->term > MAX_TERM && dpp->term < 17) || dpp->term > 18 ||
            ((wps->wphdr.flags & MONO_DATA) && dpp->term < 0))
                return FALSE;
    }

    return TRUE;
}

// Read decorrelation weights from specified metadata block into the
// decorr_passes array. The weights range +/-1024, but are rounded and
// truncated to fit in signed chars for metadata storage. Weights are
// separate for the two channels and are specified from the "last" term
// (first during encode). Unspecified weights are set to zero.

int read_decorr_weights (WavpackStream *wps, WavpackMetadata *wpmd)
{
    int termcnt = wpmd->byte_length, tcount;
    char *byteptr = wpmd->data;
    struct decorr_pass *dpp;

    if (!(wps->wphdr.flags & MONO_DATA))
        termcnt /= 2;

    if (termcnt > wps->num_terms)
        return FALSE;

    for (tcount = wps->num_terms, dpp = wps->decorr_passes; tcount--; dpp++)
        dpp->weight_A = dpp->weight_B = 0;

    while (--dpp >= wps->decorr_passes && termcnt--) {
        dpp->weight_A = restore_weight (*byteptr++);

        if (!(wps->wphdr.flags & MONO_DATA))
            dpp->weight_B = restore_weight (*byteptr++);
    }

    return TRUE;
}

// Read decorrelation samples from specified metadata block into the
// decorr_passes array. The samples are signed 32-bit values, but are
// converted to signed log2 values for storage in metadata. Values are
// stored for both channels and are specified from the "last" term
// (first during encode) with unspecified samples set to zero. The
// number of samples stored varies with the actual term value, so
// those must obviously come first in the metadata.

int read_decorr_samples (WavpackStream *wps, WavpackMetadata *wpmd)
{
    unsigned char *byteptr = wpmd->data;
    unsigned char *endptr = byteptr + wpmd->byte_length;
    struct decorr_pass *dpp;
    int tcount;

    for (tcount = wps->num_terms, dpp = wps->decorr_passes; tcount--; dpp++) {
        CLEAR (dpp->samples_A);
        CLEAR (dpp->samples_B);
    }

#ifdef LARGE_HEADER
    if (wps->wphdr.version == 0x402 && (wps->wphdr.flags & HYBRID_FLAG)) {
        if (byteptr + (wps->wphdr.flags & MONO_DATA ? 2 : 4) > endptr)
            return FALSE;

        wps->dc.error [0] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
        byteptr += 2;

        if (!(wps->wphdr.flags & MONO_DATA)) {
            wps->dc.error [1] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
            byteptr += 2;
        }
    }
#endif

    while (dpp-- > wps->decorr_passes && byteptr < endptr)
        if (dpp->term > MAX_TERM) {
            if (byteptr + (wps->wphdr.flags & MONO_DATA ? 4 : 8) > endptr)
                return FALSE;

            dpp->samples_A [0] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
            dpp->samples_A [1] = wp_exp2s ((int16_t)(byteptr [2] + (byteptr [3] << 8)));
            byteptr += 4;

            if (!(wps->wphdr.flags & MONO_DATA)) {
                dpp->samples_B [0] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
                dpp->samples_B [1] = wp_exp2s ((int16_t)(byteptr [2] + (byteptr [3] << 8)));
                byteptr += 4;
            }
        }
        else if (dpp->term < 0) {
            if (byteptr + 4 > endptr)
                return FALSE;

            dpp->samples_A [0] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
            dpp->samples_B [0] = wp_exp2s ((int16_t)(byteptr [2] + (byteptr [3] << 8)));
            byteptr += 4;
        }
        else {
            int m = 0, cnt = dpp->term;

            while (cnt--) {
                if (byteptr + (wps->wphdr.flags & MONO_DATA ? 2 : 4) > endptr)
                    return FALSE;

                dpp->samples_A [m] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
                byteptr += 2;

                if (!(wps->wphdr.flags & MONO_DATA)) {
                    dpp->samples_B [m] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
                    byteptr += 2;
                }

                m++;
            }
        }

    return byteptr == endptr;
}

int read_decorr_combined (WavpackStream *wps, WavpackMetadata *wpmd)
{
    signed char *byteptr = wpmd->data, *saveptr;
    signed char *endptr = byteptr + wpmd->byte_length;
    signed char *expanded_data;
    struct decorr_pass *dpp;
    int termcnt, i;

    if (wpmd->byte_length < 1)
        return FALSE;

    // first, read the terms

    termcnt = wps->num_terms = *byteptr & 0x1f;

    if (termcnt > MAX_NTERMS)
        return FALSE;
    else if (!termcnt)
        return TRUE;

    if (*byteptr & 0x80) {
        int delta;

        if ((*byteptr++ & 0x60) || endptr - byteptr < termcnt / 2 + 1)
            return FALSE;

        delta = *byteptr & 0x7;
        dpp = wps->decorr_passes + termcnt;
        (--dpp)->term = *byteptr++ >> 4;
        termcnt--;

        while (termcnt >= 2) {
            (--dpp)->term = *byteptr;
            (--dpp)->term = *byteptr++ >> 4;
            termcnt -= 2;
        }

        if (termcnt)
            (--dpp)->term = *byteptr++;

        for (i = 0; i < wps->num_terms; ++i) {
            dpp->term = ((dpp->term & 0xF) < 12) ? (dpp->term & 0xF) - 3 : (dpp->term & 0xF) + 3;

            if (!dpp->term || dpp->term < -3 || (dpp->term > MAX_TERM && dpp->term < 17) || dpp->term > 18 ||
                ((wps->wphdr.flags & MONO_DATA) && dpp->term < 0))
                    return FALSE;

            dpp++->delta = delta;
        }
    }
    else {
        int neg_term_replace = 0, table_index = (unsigned char)(byteptr [1]);
        const signed char *spec_terms;
        const WavpackDecorrSpec *spec;

        if (wps->wphdr.flags & MONO_DATA)
            neg_term_replace = 1;
        else if (!(wps->wphdr.flags & CROSS_DECORR))
            neg_term_replace = -3;

        if ((*byteptr & 0x60) == 0x00) {
            if (table_index >= NUM_FAST_SPECS)
                return FALSE;

            spec = fast_specs + table_index;
        }
        else if ((*byteptr & 0x60) == 0x20) {
            if (table_index >= NUM_DEFAULT_SPECS)
                return FALSE;

            spec = default_specs + table_index;
        }
        else if ((*byteptr & 0x60) == 0x40) {
            if (table_index >= NUM_HIGH_SPECS)
                return FALSE;

            spec = high_specs + table_index;
        }
        else {
            if (table_index >= NUM_VERY_HIGH_SPECS)
                return FALSE;

            spec = very_high_specs + table_index;
        }

        spec_terms = spec->terms;

        for (dpp = wps->decorr_passes + termcnt - 1; termcnt--; dpp--, spec_terms++) {
            dpp->term = (neg_term_replace && *spec_terms < 0) ? neg_term_replace : *spec_terms;
            dpp->delta = spec->delta;

            if (!dpp->term || dpp->term < -3 || (dpp->term > MAX_TERM && dpp->term < 17) || dpp->term > 18 ||
                ((wps->wphdr.flags & MONO_DATA) && dpp->term < 0))
                    return FALSE;
        }

        byteptr += 2;
    }

    // next, read the weights (which are stored 2 per byte)

    termcnt = (wps->wphdr.flags & MONO_DATA) ? (wps->num_terms + 1) & ~1 : wps->num_terms * 2;
    expanded_data = malloc (termcnt);

    for (i = 0; i < termcnt; i++)
        if (i & 1)
            expanded_data [i] = *byteptr++ << 4;
        else
            expanded_data [i] = *byteptr;

    saveptr = byteptr;
    byteptr = expanded_data;

    if (wps->wphdr.flags & MONO_DATA) {
        if (termcnt == wps->num_terms + 1)
            termcnt--;
    }
    else
        termcnt /= 2;

    if (termcnt != wps->num_terms) {
        free (expanded_data);
        return FALSE;
    }

    // first we reset all the terms to "0"

    for (i = wps->num_terms, dpp = wps->decorr_passes; i--; dpp++)
        dpp->weight_A = dpp->weight_B = restore_weight_nybble (0);

    // then we just write to "termcnt" values (*2 for stereo), starting at the end

    while (--dpp >= wps->decorr_passes && termcnt--) {
        dpp->weight_A = restore_weight_nybble (*byteptr++);

        if (!(wps->wphdr.flags & MONO_DATA))
            dpp->weight_B = restore_weight_nybble (*byteptr++);
    }

    free (expanded_data);
    byteptr = saveptr;

    // finally, handle the sample history

    for (termcnt = wps->num_terms, dpp = wps->decorr_passes; termcnt--; dpp++) {
        CLEAR (dpp->samples_A);
        CLEAR (dpp->samples_B);
    }

    while (dpp-- > wps->decorr_passes && byteptr < endptr)
        if (dpp->term > MAX_TERM) {
            if (byteptr + (wps->wphdr.flags & MONO_DATA ? 2 : 4) > endptr)
                return FALSE;

            dpp->samples_A [0] = wp_exp2_schar (*byteptr++);
            dpp->samples_A [1] = wp_exp2_schar (*byteptr++);

            if (!(wps->wphdr.flags & MONO_DATA)) {
                dpp->samples_B [0] = wp_exp2_schar (*byteptr++);
                dpp->samples_B [1] = wp_exp2_schar (*byteptr++);
            }
        }
        else if (dpp->term < 0) {
            if (byteptr + 2 > endptr)
                return FALSE;

            dpp->samples_A [0] = wp_exp2_schar (*byteptr++);
            dpp->samples_B [0] = wp_exp2_schar (*byteptr++);
        }
        else {
            int m = 0, cnt = dpp->term;

            while (cnt--) {
                if (byteptr + (wps->wphdr.flags & MONO_DATA ? 1 : 2) > endptr)
                    return FALSE;

                dpp->samples_A [m] = wp_exp2_schar (*byteptr++);

                if (!(wps->wphdr.flags & MONO_DATA))
                    dpp->samples_B [m] = wp_exp2_schar (*byteptr++);

                m++;
            }
        }

    return byteptr == endptr;
}

// Read the shaping weights from specified metadata block into the
// WavpackStream structure. Note that there must be two values (even
// for mono streams) and that the values are stored in the same
// manner as decorrelation weights. These would normally be read from
// the "correction" file and are used for lossless reconstruction of
// hybrid data.

int read_shaping_info (WavpackStream *wps, WavpackMetadata *wpmd)
{
    if (wpmd->byte_length == 2) {
        char *byteptr = wpmd->data;

        wps->dc.shaping_acc [0] = (int32_t) restore_weight (*byteptr++) << 16;
        wps->dc.shaping_acc [1] = (int32_t) restore_weight (*byteptr++) << 16;
        return TRUE;
    }
    else if (wpmd->byte_length >= (wps->wphdr.flags & MONO_DATA ? 4 : 8)) {
        unsigned char *byteptr = wpmd->data;

        wps->dc.error [0] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
        wps->dc.shaping_acc [0] = wp_exp2s ((int16_t)(byteptr [2] + (byteptr [3] << 8)));
        byteptr += 4;

        if (!(wps->wphdr.flags & MONO_DATA)) {
            wps->dc.error [1] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));
            wps->dc.shaping_acc [1] = wp_exp2s ((int16_t)(byteptr [2] + (byteptr [3] << 8)));
            byteptr += 4;
        }

        if (wpmd->byte_length == (wps->wphdr.flags & MONO_DATA ? 6 : 12)) {
            wps->dc.shaping_delta [0] = wp_exp2s ((int16_t)(byteptr [0] + (byteptr [1] << 8)));

            if (!(wps->wphdr.flags & MONO_DATA))
                wps->dc.shaping_delta [1] = wp_exp2s ((int16_t)(byteptr [2] + (byteptr [3] << 8)));
        }

        return TRUE;
    }

    return FALSE;
}
