############################################################################
##                           **** WAVPACK ****                            ##
##                  Hybrid Lossless Wavefile Compressor                   ##
##              Copyright (c) 1998 - 2015 Conifer Software.               ##
##                          All Rights Reserved.                          ##
##      Distributed under the BSD Software License (see license.txt)      ##
############################################################################

        .intel_syntax noprefix
        .text
        .globl  pack_decorr_stereo_pass_cont_rev_x86
        .globl  pack_decorr_stereo_pass_cont_x86
        .globl  pack_decorr_mono_buffer_x86
        .globl  log2buffer_x86

# This module contains X86 assembly optimized versions of functions required
# to encode WavPack files.

# These are assembly optimized version of the following WavPack functions:
#
# void pack_decorr_stereo_pass_cont (
#   struct decorr_pass *dpp,
#   int32_t *in_buffer,
#   int32_t *out_buffer,
#   int32_t sample_count);
#
# void pack_decorr_stereo_pass_cont_rev (
#   struct decorr_pass *dpp,
#   int32_t *in_buffer,
#   int32_t *out_buffer,
#   int32_t sample_count);
#
# It performs a single pass of stereo decorrelation, transfering from the
# input buffer to the output buffer. Note that this version of the function
# requires that the up to 8 previous (depending on dpp->term) stereo samples
# are visible and correct. In other words, it ignores the "samples_*"
# fields in the decorr_pass structure and gets the history data directly
# from the source buffer. It does, however, return the appropriate history
# samples to the decorr_pass structure before returning.
#
# This is written to work on an IA-32 processor and uses the MMX extensions
# to improve the performance by processing both stereo channels together.
# It is based on the original MMX code written by Joachim Henke that used
# MMX intrinsics called from C. Many thanks to Joachim for that!
#
# No additional stack space is used; all storage is done in registers. The
# arguments on entry:
#
#   struct decorr_pass *dpp     [ebp+8]
#   int32_t *in_buffer          [ebp+12]
#   int32_t *out_buffer         [ebp+16]
#   int32_t sample_count        [ebp+20]
#
# During the processing loops, the following registers are used:
#
#   edi         input buffer pointer
#   esi         direction (-8 forward, +8 reverse)
#   ebx         delta from input to output buffer
#   ecx         sample count
#   edx         sign (dir) * term * -8 (terms 1-8 only)
#   mm0, mm1    scratch
#   mm2         original sample values
#   mm3         correlation samples
#   mm4         weight sums
#   mm5         weights
#   mm6         delta
#   mm7         512 (for rounding)
#

pack_decorr_stereo_pass_cont_rev_x86:
        push    ebp
        mov     ebp, esp
        push    ebx                         # save the registers that we need to
        push    esi
        push    edi

        mov     esi, 8                      # esi indicates direction (inverted)
        jmp     start

pack_decorr_stereo_pass_cont_x86:
        push    ebp
        mov     ebp, esp
        push    ebx                         # save the registers that we need to
        push    esi
        push    edi

        mov     esi, -8                     # esi indicates direction (inverted)

start:  mov     eax, 512
        movd    mm7, eax
        punpckldq mm7, mm7                  # mm7 = round (512)

        mov     eax, [ebp+8]                # access dpp
        mov     eax, [eax+4]
        movd    mm6, eax
        punpckldq mm6, mm6                  # mm6 = delta (0-7)

        mov     eax, [ebp+8]                # access dpp
        movq    mm5, [eax+8]                # mm5 = weight_AB
        movq    mm4, [eax+88]               # mm4 = sum_AB

        mov     edi, [ebp+12]               # edi = in_buffer
        mov     ebx, [ebp+16]
        sub     ebx, edi                    # ebx = delta to output buffer

        mov     ecx, [ebp+20]               # ecx = sample_count
        test    ecx, ecx
        jz      done

        mov     eax, [ebp+8]                # *eax = dpp
        mov     eax, [eax]                  # get term and vector to correct loop
        cmp     eax, 17
        je      term_17_loop
        cmp     eax, 18
        je      term_18_loop
        cmp     eax, -1
        je      term_minus_1_loop
        cmp     eax, -2
        je      term_minus_2_loop
        cmp     eax, -3
        je      term_minus_3_loop

        sal     eax, 3
        mov     edx, eax                    # edx = term * 8 to index correlation sample
        test    esi, esi                    # test direction
        jns     default_term_loop
        neg     edx
        jmp     default_term_loop

        .align  64

default_term_loop:
        movq    mm3, [edi+edx]              # mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  # mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    # add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    # add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              # store result
        pxor    mm0, mm2
        psrad   mm0, 31                     # mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    # mm1 = zero
        pcmpeqd mm2, mm1                    # mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    # mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    # mm2 = 1s if either was zero
        pandn   mm2, mm6                    # mask delta with zeros check
        pxor    mm5, mm0
        paddd   mm5, mm2                    # and add to weight_AB
        pxor    mm5, mm0
        paddd   mm4, mm5                    # add weights to sum
        dec     ecx
        jnz     default_term_loop

        mov     eax, [ebp+8]                # access dpp
        movq    [eax+8], mm5                # put weight_AB back
        movq    [eax+88], mm4               # put sum_AB back
        emms

        mov     edx, [ebp+8]                # access dpp with edx
        mov     ecx, [edx]                  # ecx = dpp->term

default_store_samples:
        dec     ecx
        add     edi, esi                    # back up one full sample
        mov     eax, [edi+4]
        mov     [edx+ecx*4+48], eax         # store samples_B [ecx]
        mov     eax, [edi]
        mov     [edx+ecx*4+16], eax         # store samples_A [ecx]
        test    ecx, ecx
        jnz     default_store_samples
        jmp     done

        .align  64

term_17_loop:
        movq    mm3, [edi+esi]              # get previous calculated value
        paddd   mm3, mm3
        psubd   mm3, [edi+esi*2]

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  # mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    # add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    # add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              # store result
        pxor    mm0, mm2
        psrad   mm0, 31                     # mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    # mm1 = zero
        pcmpeqd mm2, mm1                    # mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    # mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    # mm2 = 1s if either was zero
        pandn   mm2, mm6                    # mask delta with zeros check
        pxor    mm5, mm0
        paddd   mm5, mm2                    # and add to weight_AB
        pxor    mm5, mm0
        paddd   mm4, mm5                    # add weights to sum
        dec     ecx
        jnz     term_17_loop

        mov     eax, [ebp+8]                # access dpp
        movq    [eax+8], mm5                # put weight_AB back
        movq    [eax+88], mm4               # put sum_AB back
        emms
        jmp     term_1718_common_store

        .align  64

term_18_loop:
        movq    mm3, [edi+esi]              # get previous calculated value
        movq    mm0, mm3
        psubd   mm3, [edi+esi*2]
        psrad   mm3, 1
        paddd   mm3, mm0                    # mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  # mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    # add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    # add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              # store result
        pxor    mm0, mm2
        psrad   mm0, 31                     # mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    # mm1 = zero
        pcmpeqd mm2, mm1                    # mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    # mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    # mm2 = 1s if either was zero
        pandn   mm2, mm6                    # mask delta with zeros check
        pxor    mm5, mm0
        paddd   mm5, mm2                    # and add to weight_AB
        pxor    mm5, mm0
        dec     ecx
        paddd   mm4, mm5                    # add weights to sum
        jnz     term_18_loop

        mov     eax, [ebp+8]                # access dpp
        movq    [eax+8], mm5                # put weight_AB back
        movq    [eax+88], mm4               # put sum_AB back
        emms

term_1718_common_store:

        mov     eax, [ebp+8]                # access dpp
        add     edi, esi                    # back up a full sample
        mov     edx, [edi+4]                # dpp->samples_B [0] = iptr [-1];
        mov     [eax+48], edx
        mov     edx, [edi]                  # dpp->samples_A [0] = iptr [-2];
        mov     [eax+16], edx
        add     edi, esi                    # back up another sample
        mov     edx, [edi+4]                # dpp->samples_B [1] = iptr [-3];
        mov     [eax+52], edx
        mov     edx, [edi]                  # dpp->samples_A [1] = iptr [-4];
        mov     [eax+20], edx
        jmp     done

        .align  64

term_minus_1_loop:
        movq    mm3, [edi+esi]              # mm3 = previous calculated value
        movq    mm2, [edi]                  # mm2 = left_right
        psrlq   mm3, 32
        punpckldq mm3, mm2                  # mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    # add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    # add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              # store result
        pxor    mm0, mm2
        psrad   mm0, 31                     # mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    # mm1 = zero
        pcmpeqd mm2, mm1                    # mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    # mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    # mm2 = 1s if either was zero
        pandn   mm2, mm6                    # mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddd   mm5, mm1
        paddusw mm5, mm2                    # and add to weight_AB
        psubd   mm5, mm1
        pxor    mm5, mm0
        paddd   mm4, mm5                    # add weights to sum
        dec     ecx
        jnz     term_minus_1_loop

        mov     eax, [ebp+8]                # access dpp
        movq    [eax+8], mm5                # put weight_AB back
        movq    [eax+88], mm4               # put sum_AB back
        emms

        add     edi, esi                    # back up a full sample
        mov     edx, [edi+4]                # dpp->samples_A [0] = iptr [-1];
        mov     eax, [ebp+8]
        mov     [eax+16], edx
        jmp     done

        .align  64

term_minus_2_loop:
        movq    mm2, [edi]                  # mm2 = left_right
        movq    mm3, mm2                    # mm3 = swap dwords
        psrlq   mm3, 32
        punpckldq mm3, [edi+esi]            # mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    # add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    # add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              # store result
        pxor    mm0, mm2
        psrad   mm0, 31                     # mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    # mm1 = zero
        pcmpeqd mm2, mm1                    # mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    # mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    # mm2 = 1s if either was zero
        pandn   mm2, mm6                    # mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddd   mm5, mm1
        paddusw mm5, mm2                    # and add to weight_AB
        psubd   mm5, mm1
        pxor    mm5, mm0
        paddd   mm4, mm5                    # add weights to sum
        dec     ecx
        jnz     term_minus_2_loop

        mov     eax, [ebp+8]                # access dpp
        movq    [eax+8], mm5                # put weight_AB back
        movq    [eax+88], mm4               # put sum_AB back
        emms

        add     edi, esi                    # back up a full sample
        mov     edx, [edi]                  # dpp->samples_B [0] = iptr [-2];
        mov     eax, [ebp+8]
        mov     [eax+48], edx
        jmp     done

        .align  64

term_minus_3_loop:
        movq    mm0, [edi+esi]              # mm0 = previous calculated value
        movq    mm3, mm0                    # mm3 = swap dwords
        psrlq   mm3, 32
        punpckldq mm3, mm0                  # mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  # mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    # add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    # add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              # store result
        pxor    mm0, mm2
        psrad   mm0, 31                     # mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    # mm1 = zero
        pcmpeqd mm2, mm1                    # mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    # mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    # mm2 = 1s if either was zero
        pandn   mm2, mm6                    # mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddd   mm5, mm1
        paddusw mm5, mm2                    # and add to weight_AB
        psubd   mm5, mm1
        pxor    mm5, mm0
        paddd   mm4, mm5                    # add weights to sum
        dec     ecx
        jnz     term_minus_3_loop

        mov     eax, [ebp+8]                # access dpp
        movq    [eax+8], mm5                # put weight_AB back
        movq    [eax+88], mm4               # put sum_AB back
        emms

        add     edi, esi                    # back up a full sample
        mov     edx, [edi+4]                # dpp->samples_A [0] = iptr [-1];
        mov     eax, [ebp+8]
        mov     [eax+16], edx
        mov     edx, [edi]                  # dpp->samples_B [0] = iptr [-2];
        mov     [eax+48], edx

done:   pop     edi
        pop     esi
        pop     ebx
        leave
        ret


# This is an assembly optimized version of the following WavPack function:
#
# void decorr_mono_buffer (int32_t *buffer,
#                          struct decorr_pass *decorr_passes,
#                          int32_t num_terms,
#                          int32_t sample_count)
#
# Decorrelate a buffer of mono samples, in place, as specified by the array
# of decorr_pass structures. Note that this function does NOT return the
# dpp->samples_X[] values in the "normalized" positions for terms 1-8, so if
# the number of samples is not a multiple of MAX_TERM, these must be moved if
# they are to be used somewhere else.
#
# By using the overflow detection of the multiply instruction, it detects
# when the "long_math" varient is required and automatically branches to it
# for the rest of the loop.
#
# This is written to work on an IA-32 processor. The arguments are on the
# stack at these locations (after 5 pushes, we do not use ebp as a base
# pointer):
#
#   int32_t *buffer             [esp+24]
#   struct decorr_pass *dpp     [esp+28]
#   int32_t num_terms           [esp+32]
#   int32_t sample_count        [esp+36]
#
# register usage:
#
# ecx = sample being decorrelated
# esi = sample up counter
# edi = *buffer
# ebp = *dpp
#
# stack usage:
#
# [esp+0] = dpp end ptr
#

pack_decorr_mono_buffer_x86:
        push    ebp                         # save the resgister that we need to
        push    ebx
        push    esi
        push    edi
        push    eax                         # this will be dpp end ptr

        mov     edx, [esp+32]               # get number of terms
        imul    eax, edx, 96                # calculate & store termination check ptr
        add     eax, [esp+28]
        mov     [esp], eax

        cmp     DWORD PTR [esp+36], 0       # test & handle zero sample count & zero term count
        jz      nothing_to_do
        test    edx, edx
        jz      nothing_to_do

        mov     edi, [esp+24]
        mov     ebp, [esp+28]
        xor     esi, esi                     # up counter = 0
        jmp     decorrelate_loop

nothing_to_do:
        pop     eax
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        .align  64

decorrelate_loop:
        mov     ecx, [edi+esi*4]             # ecx is the sample we're decorrelating
1:      mov     dl, [ebp]
        cmp     dl, 17
        jge     3f

        mov     eax, esi
        and     eax, 7
        mov     ebx, [ebp+16+eax*4]
        add     al, dl
        and     al, 7
        mov     [ebp+16+eax*4], ecx
        jmp     decorr_continue

        .align  4
3:      mov     edx, [ebp+16]
        mov     [ebp+16], ecx
        je      4f
        lea     ebx, [edx+edx*2]
        sub     ebx, [ebp+20]
        sar     ebx, 1
        mov     [ebp+20], edx
        jmp     decorr_continue

        .align  4
4:      lea     ebx, [edx+edx]
        sub     ebx, [ebp+20]
        mov     [ebp+20], edx

decorr_continue:
        mov     eax, [ebp+8]
        mov     edx, eax
        imul    eax, ebx
        jo      long_decorr_continue        # on overflow jump to other version
        sar     eax, 10
        sbb     ecx, eax
        je      2f
        test    ebx, ebx
        je      2f
        xor     ebx, ecx
        sar     ebx, 31
        xor     edx, ebx
        add     edx, [ebp+4]
        xor     edx, ebx
        mov     [ebp+8], edx
2:      add     ebp, 96
        cmp     ebp, [esp]
        jnz     1b

        mov     [edi+esi*4], ecx            # store completed sample
        mov     ebp, [esp+28]               # reload decorr_passes pointer to first term
        inc     esi                         # increment sample index
        cmp     esi, [esp+36]
        jnz     decorrelate_loop

        pop     eax
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        .align  4

long_decorr_loop:
        mov     dl, [ebp]
        cmp     dl, 17
        jge     3f

        mov     eax, esi
        and     eax, 7
        mov     ebx, [ebp+16+eax*4]
        add     al, dl
        and     al, 7
        mov     [ebp+16+eax*4], ecx
        jmp     long_decorr_continue

        .align  4
3:      mov     edx, [ebp+16]
        mov     [ebp+16], ecx
        je      4f
        lea     ebx, [edx+edx*2]
        sub     ebx, [ebp+20]
        sar     ebx, 1
        mov     [ebp+20], edx
        jmp     long_decorr_continue

        .align  4
4:      lea     ebx, [edx+edx]
        sub     ebx, [ebp+20]
        mov     [ebp+20], edx

long_decorr_continue:
        mov     eax, [ebp+8]
        imul    ebx
        shr     eax, 10
        sbb     ecx, eax
        shl     edx, 22
        sub     ecx, edx
        je      2f
        test    ebx, ebx
        je      2f
        xor     ebx, ecx
        sar     ebx, 31
        mov     eax, [ebp+8]
        xor     eax, ebx
        add     eax, [ebp+4]
        xor     eax, ebx
        mov     [ebp+8], eax
2:      add     ebp, 96
        cmp     ebp, [esp]
        jnz     long_decorr_loop

        mov     [edi+esi*4], ecx            # store completed sample
        mov     ebp, [esp+28]               # reload decorr_passes pointer to first term
        inc     esi                         # increment sample index
        cmp     esi, [esp+36]
        jnz     decorrelate_loop            # loop all the way back this time

        pop     eax
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret


# This is an assembly optimized version of the following WavPack function:
#
# uint32_t log2buffer (int32_t *samples, uint32_t num_samples, int limit);
#
# This function scans a buffer of 32-bit ints and accumulates the total
# log2 value of all the samples. This is useful for determining maximum
# compression because the bitstream storage required for entropy coding
# is proportional to the base 2 log of the samples.
#
# This is written to work on all IA-32 processors (i386, i486, etc.)
#
# No additional stack space is used; all storage is done in registers. The
# arguments on entry:
#
#   int32_t *samples            [ebp+8]
#   uint32_t num_samples        [ebp+12]
#   int limit                   [ebp+16]
#
# During the processing loops, the following registers are used:
#
#   esi             input buffer pointer
#   edi             sum accumulator
#   ebx             sample count
#   ebp             limit (if specified non-zero)
#   eax,ecx,edx     scratch
#

        .align  256

log2_table:
        .byte   0x00, 0x01, 0x03, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x14, 0x15
        .byte   0x16, 0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x1e, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a
        .byte   0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x32, 0x33, 0x34, 0x36, 0x37, 0x38, 0x39, 0x3b, 0x3c, 0x3d, 0x3e
        .byte   0x3f, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4d, 0x4e, 0x4f, 0x50, 0x51
        .byte   0x52, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63
        .byte   0x64, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x74, 0x75
        .byte   0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85
        .byte   0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95
        .byte   0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4
        .byte   0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb2
        .byte   0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc0
        .byte   0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcb, 0xcc, 0xcd, 0xce
        .byte   0xcf, 0xd0, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd8, 0xd9, 0xda, 0xdb
        .byte   0xdc, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe4, 0xe5, 0xe6, 0xe7, 0xe7
        .byte   0xe8, 0xe9, 0xea, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xee, 0xef, 0xf0, 0xf1, 0xf1, 0xf2, 0xf3, 0xf4
        .byte   0xf4, 0xf5, 0xf6, 0xf7, 0xf7, 0xf8, 0xf9, 0xf9, 0xfa, 0xfb, 0xfc, 0xfc, 0xfd, 0xfe, 0xff, 0xff

log2buffer_x86:
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        cld

        mov     esi, [ebp+8]                # esi = sample source pointer
        xor     edi, edi                    # edi = 0 (accumulator)
        mov     ebx, [ebp+12]               # ebx = num_samples
        test    ebx, ebx                    # exit now if none, sum = 0
        jz      normal_exit

        mov     ebp, [ebp+16]               # ebp = limit
        test    ebp, ebp                    # we have separate loops for limit and no limit
        jz      no_limit_loop
        jmp     limit_loop

        .align  64

limit_loop:
        mov     eax, [esi]                  # get next sample into eax
        cdq                                 # edx = sign of sample (for abs)
        add     esi, 4
        xor     eax, edx
        sub     eax, edx
        je      L40                         # skip if sample was zero
        mov     edx, eax                    # move to edx and apply rounding
        shr     eax, 9
        add     edx, eax
        bsr     ecx, edx                    # ecx = MSB set in sample (0 - 31)
        lea     eax, [ecx+1]                # eax = number used bits in sample (1 - 32)
        sub     ecx, 8                      # ecx = shift right amount (-8 to 23)
        ror     edx, cl                     # use rotate to do "signed" shift 
        sal     eax, 8                      # move nbits to integer portion of log
        movzx   edx, dl                     # dl = mantissa, look up log fraction in table 
        mov     al, [log2_table+edx]        # eax = combined integer and fraction for full log
        add     edi, eax                    # add to running sum and compare to limit
        cmp     eax, ebp
        jge     limit_exceeded
L40:    sub     ebx, 1                      # loop back if more samples
        jne     limit_loop
        jmp     normal_exit

        .align  64

no_limit_loop:
        mov     eax, [esi]                  # get next sample into eax
        cdq                                 # edx = sign of sample (for abs)
        add     esi, 4
        xor     eax, edx
        sub     eax, edx
        je      L45                         # skip if sample was zero
        mov     edx, eax                    # move to edx and apply rounding
        shr     eax, 9
        add     edx, eax
        bsr     ecx, edx                    # ecx = MSB set in sample (0 - 31)
        lea     eax, [ecx+1]                # eax = number used bits in sample (1 - 32)
        sub     ecx, 8                      # ecx = shift right amount (-8 to 23)
        ror     edx, cl                     # use rotate to do "signed" shift 
        sal     eax, 8                      # move nbits to integer portion of log
        movzx   edx, dl                     # dl = mantissa, look up log fraction in table 
        mov     al, [log2_table+edx]        # eax = combined integer and fraction for full log
        add     edi, eax                    # add to running sum
L45:    sub     ebx, 1                      # loop back if more samples
        jne     no_limit_loop
        jmp     normal_exit

limit_exceeded:
        mov     edi, -1                     # -1 return means log limit exceeded
normal_exit:
        mov     eax, edi                    # move sum accumulator into eax for return
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

