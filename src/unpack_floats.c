////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// unpack_floats.c

// This module deals with the restoration of floating-point data. Note that no
// floating point math is involved here...the values are only processed with
// the macros that directly access the mantissa, exponent, and sign fields.
// That's why we use the f32 type instead of the built-in float type.

#include <stdlib.h>

#include "wavpack_local.h"

static void float_values_nowvx (WavpackStream *wps, int32_t *values, int32_t num_values);

void float_values (WavpackStream *wps, int32_t *values, int32_t num_values)
{
    int min_shifted_zeros = wps->float_min_shifted_zeros;
    int max_shifted_ones = wps->float_max_shifted_ones;
    uint32_t crc = wps->crc_x;

    if (!bs_is_open (&wps->wvxbits)) {
        float_values_nowvx (wps, values, num_values);
        return;
    }

    while (num_values--) {
        int shift_count = 0, exp = wps->float_max_exp;
        f32 outval = 0;
        uint32_t temp;

        if (*values == 0) {
            if (wps->float_flags & FLOAT_ZEROS_SENT) {
                if (getbit (&wps->wvxbits)) {
                    getbits (&temp, 23, &wps->wvxbits);
                    set_mantissa (outval, temp);

                    if (exp >= 25) {
                        getbits (&temp, 8, &wps->wvxbits);
                        set_exponent (outval, temp);
                    }

                    set_sign (outval, getbit (&wps->wvxbits));
                }
                else if (wps->float_flags & FLOAT_NEG_ZEROS)
                    set_sign (outval, getbit (&wps->wvxbits));
            }
        }
        else {
            *(uint32_t*)values <<= (wps->float_shift & 0x1f);

            if (*values < 0) {
                *values = -*values;
                set_sign (outval, 1);
            }

            if (*values == 0x1000000) {
                if (getbit (&wps->wvxbits)) {
                    getbits (&temp, 23, &wps->wvxbits);
                    set_mantissa (outval, temp);
                }

                set_exponent (outval, 255);
            }
            else {
                if (exp)
                    while (!(*values & 0x800000) && --exp) {
                        shift_count++;
                        *(uint32_t*)values <<= 1;
                    }

                if (shift_count &= 0x1f) {
                    if ((wps->float_flags & FLOAT_SHIFT_ONES) ||
                        ((wps->float_flags & FLOAT_SHIFT_SAME) && getbit (&wps->wvxbits)))
                            *values |= ((1U << shift_count) - 1);
                    else if (wps->float_flags & FLOAT_SHIFT_SENT) {
                        int32_t mask = (1U << shift_count) - 1;
                        int num_zeros = 0;

                        if (max_shifted_ones && shift_count > max_shifted_ones)
                            num_zeros = shift_count - max_shifted_ones;

                        if (min_shifted_zeros > num_zeros)
                            num_zeros = (min_shifted_zeros > shift_count) ? shift_count : min_shifted_zeros;

                        if ((shift_count -= num_zeros) > 0) {
                            getbits (&temp, shift_count, &wps->wvxbits);
                            *values |= (temp << num_zeros) & mask;
                        }
                    }
                }

                set_mantissa (outval, *values);
                set_exponent (outval, exp);
            }
        }

        crc = crc * 27 + get_mantissa (outval) * 9 + get_exponent (outval) * 3 + get_sign (outval);
        * (f32 *) values++ = outval;
    }

    wps->crc_x = crc;
}

static void float_values_nowvx (WavpackStream *wps, int32_t *values, int32_t num_values)
{
    while (num_values--) {
        int shift_count = 0, exp = wps->float_max_exp;
        f32 outval = 0;

        if (*values) {
            *(uint32_t*)values <<= (wps->float_shift & 0x1f);

            if (*values < 0) {
                *values = -*values;
                set_sign (outval, 1);
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
                    *(uint32_t*)values <<= 1;
                }

                if ((shift_count &= 0x1f) && (wps->float_flags & FLOAT_SHIFT_ONES))
                    *values |= ((1U << shift_count) - 1);
            }

            set_mantissa (outval, *values);
            set_exponent (outval, exp);
        }

        * (f32 *) values++ = outval;
    }
}
