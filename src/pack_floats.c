////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack_floats.c

// This module deals with the compression of floating-point data. Note that no
// floating point math is involved here...the values are only processed with
// the macros that directly access the mantissa, exponent, and sign fields.
// That's why we use the f32 type instead of the built-in float type.

#include <stdlib.h>

#include "wavpack_local.h"

int scan_float_data (WavpackStream *wps, f32 *values, int32_t num_values)
{
    int32_t shifted_ones = 0, shifted_zeros = 0, shifted_both = 0;
    int32_t false_zeros = 0, neg_zeros = 0;
    uint32_t ordata = 0, crc = 0xffffffff;
    int32_t count, value, shift_count;
    int max_exp = 0;
    f32 *dp;

    wps->float_shift = wps->float_flags = 0;

    for (dp = values, count = num_values; count--; dp++) {
        crc = crc * 27 + get_mantissa (*dp) * 9 + get_exponent (*dp) * 3 + get_sign (*dp);

        if (get_exponent (*dp) > max_exp && get_exponent (*dp) < 255)
            max_exp = get_exponent (*dp);
    }

    wps->crc_x = crc;

    for (dp = values, count = num_values; count--; dp++) {
        if (get_exponent (*dp) == 255) {
            wps->float_flags |= FLOAT_EXCEPTIONS;
            value = 0x1000000;
            shift_count = 0;
        }
        else if (get_exponent (*dp)) {
            shift_count = max_exp - get_exponent (*dp);
            value = 0x800000 + get_mantissa (*dp);
        }
        else {
            shift_count = max_exp ? max_exp - 1 : 0;
            value = get_mantissa (*dp);

//          if (get_mantissa (*dp))
//              denormals++;
        }

        if (shift_count < 25)
            value >>= shift_count;
        else
            value = 0;

        if (!value) {
            if (get_exponent (*dp) || get_mantissa (*dp))
                ++false_zeros;
            else if (get_sign (*dp))
                ++neg_zeros;
        }
        else if (shift_count) {
            int32_t mask = (1 << shift_count) - 1;

            if (!(get_mantissa (*dp) & mask))
                shifted_zeros++;
            else if ((get_mantissa (*dp) & mask) == mask)
                shifted_ones++;
            else
                shifted_both++;
        }

        ordata |= value;
        * (int32_t *) dp = (get_sign (*dp)) ? -value : value;
    }

    wps->float_max_exp = max_exp;

    if (shifted_both)
        wps->float_flags |= FLOAT_SHIFT_SENT;
    else if (shifted_ones && !shifted_zeros)
        wps->float_flags |= FLOAT_SHIFT_ONES;
    else if (shifted_ones && shifted_zeros)
        wps->float_flags |= FLOAT_SHIFT_SAME;
    else if (ordata && !(ordata & 1)) {
        while (!(ordata & 1)) {
            wps->float_shift++;
            ordata >>= 1;
        }

        for (dp = values, count = num_values; count--; dp++)
            * (int32_t *) dp >>= wps->float_shift;
    }

    wps->wphdr.flags &= ~MAG_MASK;

    while (ordata) {
        wps->wphdr.flags += 1 << MAG_LSB;
        ordata >>= 1;
    }

    if (false_zeros || neg_zeros)
        wps->float_flags |= FLOAT_ZEROS_SENT;

    if (neg_zeros)
        wps->float_flags |= FLOAT_NEG_ZEROS;

//  error_line ("samples = %d, max exp = %d, pre-shift = %d, denormals = %d",
//      num_values, max_exp, wps->float_shift, denormals);
//  if (wps->float_flags & FLOAT_EXCEPTIONS)
//      error_line ("exceptions!");
//  error_line ("shifted ones/zeros/both = %d/%d/%d, true/neg/false zeros = %d/%d/%d",
//      shifted_ones, shifted_zeros, shifted_both, true_zeros, neg_zeros, false_zeros);

    return wps->float_flags & (FLOAT_EXCEPTIONS | FLOAT_ZEROS_SENT | FLOAT_SHIFT_SENT | FLOAT_SHIFT_SAME);
}

void send_float_data (WavpackStream *wps, f32 *values, int32_t num_values)
{
    int max_exp = wps->float_max_exp;
    int32_t count, value, shift_count;
    f32 *dp;

    for (dp = values, count = num_values; count--; dp++) {
        if (get_exponent (*dp) == 255) {
            if (get_mantissa (*dp)) {
                putbit_1 (&wps->wvxbits);
                putbits (get_mantissa (*dp), 23, &wps->wvxbits);
            }
            else {
                putbit_0 (&wps->wvxbits);
            }

            value = 0x1000000;
            shift_count = 0;
        }
        else if (get_exponent (*dp)) {
            shift_count = max_exp - get_exponent (*dp);
            value = 0x800000 + get_mantissa (*dp);
        }
        else {
            shift_count = max_exp ? max_exp - 1 : 0;
            value = get_mantissa (*dp);
        }

        if (shift_count < 25)
            value >>= shift_count;
        else
            value = 0;

        if (!value) {
            if (wps->float_flags & FLOAT_ZEROS_SENT) {
                if (get_exponent (*dp) || get_mantissa (*dp)) {
                    putbit_1 (&wps->wvxbits);
                    putbits (get_mantissa (*dp), 23, &wps->wvxbits);

                    if (max_exp >= 25) {
                        putbits (get_exponent (*dp), 8, &wps->wvxbits);
                    }

                    putbit (get_sign (*dp), &wps->wvxbits);
                }
                else {
                    putbit_0 (&wps->wvxbits);

                    if (wps->float_flags & FLOAT_NEG_ZEROS)
                        putbit (get_sign (*dp), &wps->wvxbits);
                }
            }
        }
        else if (shift_count) {
            if (wps->float_flags & FLOAT_SHIFT_SENT) {
                int32_t data = get_mantissa (*dp) & ((1 << shift_count) - 1);
                putbits (data, shift_count, &wps->wvxbits);
            }
            else if (wps->float_flags & FLOAT_SHIFT_SAME) {
                putbit (get_mantissa (*dp) & 1, &wps->wvxbits);
            }
        }
    }
}
