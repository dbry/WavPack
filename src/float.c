////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2006 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// float.c

#include "wavpack_local.h"

#include <stdlib.h>

#ifdef DEBUG_ALLOC
#define malloc malloc_db
#define realloc realloc_db
#define free free_db
void *malloc_db (uint32_t size);
void *realloc_db (void *ptr, uint32_t size);
void free_db (void *ptr);
int32_t dump_alloc (void);
#endif

#if !defined(NO_UNPACK) || defined(INFO_ONLY)

int read_float_info (WavpackStream *wps, WavpackMetadata *wpmd)
{
    int bytecnt = wpmd->byte_length;
    char *byteptr = wpmd->data;

    if (bytecnt != 4)
        return FALSE;

    wps->float_flags = *byteptr++;
    wps->float_shift = *byteptr++;
    wps->float_max_exp = *byteptr++;
    wps->float_norm_exp = *byteptr;
    return TRUE;
}

#endif

#ifndef NO_UNPACK

static void float_values_nowvx (WavpackStream *wps, int32_t *values, int32_t num_values);

void float_values (WavpackStream *wps, int32_t *values, int32_t num_values)
{
    uint32_t crc = wps->crc_x;

    if (!bs_is_open (&wps->wvxbits)) {
        float_values_nowvx (wps, values, num_values);
        return;
    }

    while (num_values--) {
        int shift_count = 0, exp = wps->float_max_exp;
        f32 outval = { 0, 0, 0 };
        uint32_t temp;

        if (*values == 0) {
            if (wps->float_flags & FLOAT_ZEROS_SENT) {
                if (getbit (&wps->wvxbits)) {
                    getbits (&temp, 23, &wps->wvxbits);
                    outval.mantissa = temp;

                    if (exp >= 25) {
                        getbits (&temp, 8, &wps->wvxbits);
                        outval.exponent = temp;
                    }

                    outval.sign = getbit (&wps->wvxbits);
                }
                else if (wps->float_flags & FLOAT_NEG_ZEROS)
                    outval.sign = getbit (&wps->wvxbits);
            }
        }
        else {
            *values <<= wps->float_shift;

            if (*values < 0) {
                *values = -*values;
                outval.sign = 1;
            }

            if (*values == 0x1000000) {
                if (getbit (&wps->wvxbits)) {
                    getbits (&temp, 23, &wps->wvxbits);
                    outval.mantissa = temp;
                }

                outval.exponent = 255;
            }
            else {
                if (exp)
                    while (!(*values & 0x800000) && --exp) {
                        shift_count++;
                        *values <<= 1;
                    }

                if (shift_count) {
                    if ((wps->float_flags & FLOAT_SHIFT_ONES) ||
                        ((wps->float_flags & FLOAT_SHIFT_SAME) && getbit (&wps->wvxbits)))
                            *values |= ((1 << shift_count) - 1);
                    else if (wps->float_flags & FLOAT_SHIFT_SENT) {
                        getbits (&temp, shift_count, &wps->wvxbits);
                        *values |= temp & ((1 << shift_count) - 1);
                    }
                }

                outval.mantissa = *values;
                outval.exponent = exp;
            }
        }

        crc = crc * 27 + outval.mantissa * 9 + outval.exponent * 3 + outval.sign;
        * (f32 *) values++ = outval;
    }

    wps->crc_x = crc;
}

static void float_values_nowvx (WavpackStream *wps, int32_t *values, int32_t num_values)
{
    while (num_values--) {
        int shift_count = 0, exp = wps->float_max_exp;
        f32 outval = { 0, 0, 0 };

        if (*values) {
            *values <<= wps->float_shift;

            if (*values < 0) {
                *values = -*values;
                outval.sign = 1;
            }

            if (*values >= 0x1000000) {
                while (*values & 0xf000000) {
                    *values >>= 1;
                    ++exp;
                }
            }
            else if (exp) {
                while (!(*values & 0x800000) && --exp) {
                    shift_count++;
                    *values <<= 1;
                }

                if (shift_count && (wps->float_flags & FLOAT_SHIFT_ONES))
                    *values |= ((1 << shift_count) - 1);
            }

            outval.mantissa = *values;
            outval.exponent = exp;
        }

        * (f32 *) values++ = outval;
    }
}

#endif

void WavpackFloatNormalize (int32_t *values, int32_t num_values, int delta_exp)
{
    f32 *fvalues = (f32 *) values, fzero = { 0, 0, 0 };
    int exp;

    if (!delta_exp)
        return;

    while (num_values--) {
        if ((exp = fvalues->exponent) == 0 || exp + delta_exp <= 0)
            *fvalues = fzero;
        else if (exp == 255 || (exp += delta_exp) >= 255) {
            fvalues->exponent = 255;
            fvalues->mantissa = 0;
        }
        else
            fvalues->exponent = exp;

        fvalues++;
    }
}
