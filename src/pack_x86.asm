;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                           **** WAVPACK ****                            ;;
;;                  Hybrid Lossless Wavefile Compressor                   ;;
;;              Copyright (c) 1998 - 2015 Conifer Software.               ;;
;;                          All Rights Reserved.                          ;;
;;      Distributed under the BSD Software License (see license.txt)      ;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

        .686
        .mmx
        .model  flat
asmcode segment page 'CODE'
        public  _pack_decorr_stereo_pass_x86
        public  _pack_decorr_stereo_pass_cont_rev_x86
        public  _pack_decorr_stereo_pass_cont_x86
        public  _pack_decorr_mono_buffer_x86
        public  _pack_cpu_has_feature_x86
        public  _log2buffer_x86

; This module contains X86 assembly optimized versions of functions required
; to encode WavPack files.

; This is an assembly optimized version of the following WavPack function:
;
; void pack_decorr_stereo_pass (
;   struct decorr_pass *dpp,
;   int32_t *buffer,
;   int32_t sample_count);
;
; It performs a single pass of stereo decorrelation, in place, as specified
; by the decorr_pass structure. Note that this function does NOT return the
; dpp->samples_X[] values in the "normalized" positions for terms 1-8, so if
; the number of samples is not a multiple of MAX_TERM, these must be moved if
; they are to be used somewhere else.
;
; This is written to work on an IA-32 processor and uses the MMX extensions
; to improve the performance by processing both stereo channels together.
; It is based on the original MMX code written by Joachim Henke that used
; MMX intrinsics called from C. Many thanks to Joachim for that!
;
; An issue with using MMX for this is that the sample history array in the
; decorr_pass structure contains separate arrays for each channel while the
; MMX code wants there to be a single array of dual samples. The fix for
; this is to convert the data in the arrays on entry and exit, and this is
; made easy by the fact that the 8 MMX regsiters hold exactly the required
; amount of data (64 bytes)!
;
; This is written to work on an IA-32 processor. The arguments are on the
; stack at these locations (after 4 pushes, we do not use ebp as a base
; pointer):
;
;   struct decorr_pass *dpp   [esp+20]
;   int32_t *buffer           [esp+24]
;   int32_t sample_count      [esp+28]
;
; During the processing loops, the following registers are used:
;
;   edi         buffer pointer
;   esi         termination buffer pointer
;   eax,ebx,edx used in default term to reduce calculation         
;   ebp         decorr_pass pointer
;   mm0, mm1    scratch
;   mm2         original sample values
;   mm3         correlation samples
;   mm4         0 (for pcmpeqd)
;   mm5         weights
;   mm6         delta
;   mm7         512 (for rounding)
;

_pack_decorr_stereo_pass_x86:
        push    ebp
        push    ebx
        push    edi
        push    esi

        mov     ebp, [esp+20]               ; ebp = *dpp
        mov     edi, [esp+24]               ; edi = buffer
        mov     esi, [esp+28]
        sal     esi, 3
        jz      bdone
        add     esi, edi                    ; esi = termination buffer pointer

        ; convert samples_A and samples_B array into samples_AB array for MMX
        ; (the MMX registers provide exactly enough storage to do this easily)

        movq        mm0, [ebp+16]
        punpckldq   mm0, [ebp+48]
        movq        mm1, [ebp+16]
        punpckhdq   mm1, [ebp+48]
        movq        mm2, [ebp+24]
        punpckldq   mm2, [ebp+56]
        movq        mm3, [ebp+24]
        punpckhdq   mm3, [ebp+56]
        movq        mm4, [ebp+32]
        punpckldq   mm4, [ebp+64]
        movq        mm5, [ebp+32]
        punpckhdq   mm5, [ebp+64]
        movq        mm6, [ebp+40]
        punpckldq   mm6, [ebp+72]
        movq        mm7, [ebp+40]
        punpckhdq   mm7, [ebp+72]

        movq    [ebp+16], mm0
        movq    [ebp+24], mm1
        movq    [ebp+32], mm2
        movq    [ebp+40], mm3
        movq    [ebp+48], mm4
        movq    [ebp+56], mm5
        movq    [ebp+64], mm6
        movq    [ebp+72], mm7

        mov     eax, 512
        movd    mm7, eax
        punpckldq mm7, mm7                  ; mm7 = round (512)

        mov     eax, [ebp+4]
        movd    mm6, eax
        punpckldq mm6, mm6                  ; mm6 = delta (0-7)

        mov     eax, 0FFFFh                 ; mask high weights to zero for PMADDWD
        movd    mm5, eax
        punpckldq mm5, mm5                  ; mm5 = weight mask 0x0000FFFF0000FFFF
        pand    mm5, [ebp+8]                ; mm5 = weight_AB masked to 16-bit

        movq    mm4, [ebp+16]               ; preload samples_AB[0]

        mov     al, [ebp]                   ; get term and vector to correct loop
        cmp     al, 17
        je      buff_term_17_loop
        cmp     al, 18
        je      buff_term_18_loop
        cmp     al, -1
        je      buff_term_minus_1_loop
        cmp     al, -2
        je      buff_term_minus_2_loop
        cmp     al, -3
        je      buff_term_minus_3_loop

        pxor    mm4, mm4                    ; mm4 = 0 (for pcmpeqd)
        xor     eax, eax
        xor     ebx, ebx
        add     bl, [ebp]
        mov     ecx, 7
        and     ebx, ecx
        jmp     buff_default_term_loop

        align  64

buff_default_term_loop:
        movq    mm2, [edi]                  ; mm2 = left_right
        movq    mm3, [ebp+16+eax*8]
        inc     eax
        and     eax, ecx
        movq    [ebp+16+ebx*8], mm2
        inc     ebx
        and     ebx, ecx

        movq    mm1, mm3
        paddd   mm1, mm1
        psrlw   mm1, 1
        pmaddwd mm1, mm5

        movq    mm0, mm3
        psrld   mm0, 15
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi], mm2                  ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        add     edi, 8
        pcmpeqd mm2, mm4                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm4                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pxor    mm5, mm0
        paddw   mm5, mm2                    ; and add to weight_AB
        pxor    mm5, mm0
        cmp     edi, esi
        jnz     buff_default_term_loop

        jmp     bdone

        align  64

buff_term_17_loop:
        movq    mm3, mm4                    ; get previous calculated value
        paddd   mm3, mm4
        psubd   mm3, [ebp+24]
        movq    [ebp+24], mm4

        movq    mm1, mm3
        paddd   mm1, mm1
        psrlw   mm1, 1
        pmaddwd mm1, mm5

        movq    mm0, mm3
        psrld   mm0, 15
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  ; mm2 = left_right
        movq    mm4, mm2
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi], mm2                  ; store result
        pxor    mm1, mm1
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        add     edi, 8
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pxor    mm5, mm0
        paddw   mm5, mm2                    ; and add to weight_AB
        pxor    mm5, mm0
        cmp     edi, esi
        jnz     buff_term_17_loop

        movq    [ebp+16], mm4               ; post-store samples_AB[0]
        jmp     bdone

        align  64

buff_term_18_loop:
        movq    mm3, mm4                    ; get previous calculated value
        psubd   mm3, [ebp+24]
        psrad   mm3, 1
        paddd   mm3, mm4                    ; mm3 = sam_AB
        movq    [ebp+24], mm4

        movq    mm1, mm3
        paddd   mm1, mm1
        psrlw   mm1, 1
        pmaddwd mm1, mm5

        movq    mm0, mm3
        psrld   mm0, 15
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  ; mm2 = left_right
        movq    mm4, mm2
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi], mm2                  ; store result
        pxor    mm1, mm1
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        add     edi, 8
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pxor    mm5, mm0
        paddw   mm5, mm2                    ; and add to weight_AB
        pxor    mm5, mm0
        cmp     edi, esi
        jnz     buff_term_18_loop

        movq    [ebp+16], mm4               ; post-store samples_AB[0]
        jmp     bdone

        align  64

buff_term_minus_1_loop:
        movq    mm3, mm4                    ; mm3 = previous calculated value
        movq    mm2, [edi]                  ; mm2 = left_right
        movq    mm4, mm2
        psrlq   mm4, 32
        punpckldq mm3, mm2                  ; mm3 = sam_AB

        movq    mm1, mm3
        paddd   mm1, mm1
        psrlw   mm1, 1
        pmaddwd mm1, mm5

        movq    mm0, mm3
        psrld   mm0, 15
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi], mm2                  ; store result
        pxor    mm1, mm1
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        add     edi, 8
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddw   mm5, mm1
        paddusw mm5, mm2                    ; and add to weight_AB
        psubw   mm5, mm1
        pxor    mm5, mm0
        cmp     edi, esi
        jnz     buff_term_minus_1_loop

        movq    [ebp+16], mm4               ; post-store samples_AB[0]
        jmp     bdone

        align  64

buff_term_minus_2_loop:
        movq    mm2, [edi]                  ; mm2 = left_right
        movq    mm3, mm2
        psrlq   mm3, 32
        por     mm3, mm4
        punpckldq mm4, mm2

        movq    mm1, mm3
        paddd   mm1, mm1
        psrlw   mm1, 1
        pmaddwd mm1, mm5

        movq    mm0, mm3
        psrld   mm0, 15
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi], mm2                  ; store result
        pxor    mm1, mm1
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        add     edi, 8
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddw   mm5, mm1
        paddusw mm5, mm2                    ; and add to weight_AB
        psubw   mm5, mm1
        pxor    mm5, mm0
        cmp     edi, esi
        jnz     buff_term_minus_2_loop

        movq    [ebp+16], mm4               ; post-store samples_AB[0]
        jmp     bdone

        align  64

buff_term_minus_3_loop:
        movq    mm2, [edi]                  ; mm2 = left_right
        movq    mm3, mm4                    ; mm3 = previous calculated value
        movq    mm4, mm2                    ; mm0 = swap dwords of new data
        psrlq   mm4, 32
        punpckldq mm4, mm2                  ; mm3 = sam_AB

        movq    mm1, mm3
        paddd   mm1, mm1
        psrlw   mm1, 1
        pmaddwd mm1, mm5

        movq    mm0, mm3
        psrld   mm0, 15
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi], mm2                  ; store result
        pxor    mm1, mm1
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        add     edi, 8
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddw   mm5, mm1
        paddusw mm5, mm2                    ; and add to weight_AB
        psubw   mm5, mm1
        pxor    mm5, mm0
        cmp     edi, esi
        jnz     buff_term_minus_3_loop

        movq    [ebp+16], mm4               ; post-store samples_AB[0]

bdone:  pslld   mm5, 16                     ; sign-extend 16-bit weights back to dwords
        psrad   mm5, 16
        movq    [ebp+8], mm5                ; put weight_AB back

        ; convert samples_AB array back into samples_A and samples_B

        movq    mm0, [ebp+16]
        movq    mm1, [ebp+24]
        movq    mm2, [ebp+32]
        movq    mm3, [ebp+40]
        movq    mm4, [ebp+48]
        movq    mm5, [ebp+56]
        movq    mm6, [ebp+64]
        movq    mm7, [ebp+72]

        movd    DWORD PTR [ebp+16], mm0
        movd    DWORD PTR [ebp+20], mm1
        movd    DWORD PTR [ebp+24], mm2
        movd    DWORD PTR [ebp+28], mm3
        movd    DWORD PTR [ebp+32], mm4
        movd    DWORD PTR [ebp+36], mm5
        movd    DWORD PTR [ebp+40], mm6
        movd    DWORD PTR [ebp+44], mm7

        punpckhdq   mm0, mm0
        punpckhdq   mm1, mm1
        punpckhdq   mm2, mm2
        punpckhdq   mm3, mm3
        punpckhdq   mm4, mm4
        punpckhdq   mm5, mm5
        punpckhdq   mm6, mm6
        punpckhdq   mm7, mm7

        movd    DWORD PTR [ebp+48], mm0
        movd    DWORD PTR [ebp+52], mm1
        movd    DWORD PTR [ebp+56], mm2
        movd    DWORD PTR [ebp+60], mm3
        movd    DWORD PTR [ebp+64], mm4
        movd    DWORD PTR [ebp+68], mm5
        movd    DWORD PTR [ebp+72], mm6
        movd    DWORD PTR [ebp+76], mm7

        emms

        pop     esi
        pop     edi
        pop     ebx
        pop     ebp
        ret

; These are assembly optimized version of the following WavPack functions:
;
; void pack_decorr_stereo_pass_cont (
;   struct decorr_pass *dpp,
;   int32_t *in_buffer,
;   int32_t *out_buffer,
;   int32_t sample_count);
;
; void pack_decorr_stereo_pass_cont_rev (
;   struct decorr_pass *dpp,
;   int32_t *in_buffer,
;   int32_t *out_buffer,
;   int32_t sample_count);
;
; It performs a single pass of stereo decorrelation, transfering from the
; input buffer to the output buffer. Note that this version of the function
; requires that the up to 8 previous (depending on dpp->term) stereo samples
; are visible and correct. In other words, it ignores the "samples_*"
; fields in the decorr_pass structure and gets the history data directly
; from the source buffer. It does, however, return the appropriate history
; samples to the decorr_pass structure before returning.
;
; This is written to work on an IA-32 processor and uses the MMX extensions
; to improve the performance by processing both stereo channels together.
; It is based on the original MMX code written by Joachim Henke that used
; MMX intrinsics called from C. Many thanks to Joachim for that!
;
; No additional stack space is used; all storage is done in registers. The
; arguments on entry:
;
;   struct decorr_pass *dpp     [ebp+8]
;   int32_t *in_buffer          [ebp+12]
;   int32_t *out_buffer         [ebp+16]
;   int32_t sample_count        [ebp+20]
;
; During the processing loops, the following registers are used:
;
;   edi         input buffer pointer
;   esi         direction (-8 forward, +8 reverse)
;   ebx         delta from input to output buffer
;   ecx         sample count
;   edx         sign (dir) * term * -8 (terms 1-8 only)
;   mm0, mm1    scratch
;   mm2         original sample values
;   mm3         correlation samples
;   mm4         weight sums
;   mm5         weights
;   mm6         delta
;   mm7         512 (for rounding)
;

_pack_decorr_stereo_pass_cont_rev_x86:
        push    ebp
        mov     ebp, esp
        push    ebx                         ; save the registers that we need to
        push    esi
        push    edi

        mov     esi, 8                      ; esi indicates direction (inverted)
        jmp     start

_pack_decorr_stereo_pass_cont_x86:
        push    ebp
        mov     ebp, esp
        push    ebx                         ; save the registers that we need to
        push    esi
        push    edi

        mov     esi, -8                     ; esi indicates direction (inverted)

start:  mov     eax, 512
        movd    mm7, eax
        punpckldq mm7, mm7                  ; mm7 = round (512)

        mov     eax, [ebp+8]                ; access dpp
        mov     eax, [eax+4]
        movd    mm6, eax
        punpckldq mm6, mm6                  ; mm6 = delta (0-7)

        mov     eax, [ebp+8]                ; access dpp
        movq    mm5, [eax+8]                ; mm5 = weight_AB
        movq    mm4, [eax+88]               ; mm4 = sum_AB

        mov     edi, [ebp+12]               ; edi = in_buffer
        mov     ebx, [ebp+16]
        sub     ebx, edi                    ; ebx = delta to output buffer

        mov     ecx, [ebp+20]               ; ecx = sample_count
        test    ecx, ecx
        jz      done

        mov     eax, [ebp+8]                ; *eax = dpp
        mov     eax, [eax]                  ; get term and vector to correct loop
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
        mov     edx, eax                    ; edx = term * 8 to index correlation sample
        test    esi, esi                    ; test direction
        jns     default_term_loop
        neg     edx
        jmp     default_term_loop

        align  64

default_term_loop:
        movq    mm3, [edi+edx]              ; mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    ; mm1 = zero
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pxor    mm5, mm0
        paddd   mm5, mm2                    ; and add to weight_AB
        pxor    mm5, mm0
        paddd   mm4, mm5                    ; add weights to sum
        dec     ecx
        jnz     default_term_loop

        mov     eax, [ebp+8]                ; access dpp
        movq    [eax+8], mm5                ; put weight_AB back
        movq    [eax+88], mm4               ; put sum_AB back
        emms

        mov     edx, [ebp+8]                ; access dpp with edx
        mov     ecx, [edx]                  ; ecx = dpp->term

default_store_samples:
        dec     ecx
        add     edi, esi                    ; back up one full sample
        mov     eax, [edi+4]
        mov     [edx+ecx*4+48], eax         ; store samples_B [ecx]
        mov     eax, [edi]
        mov     [edx+ecx*4+16], eax         ; store samples_A [ecx]
        test    ecx, ecx
        jnz     default_store_samples
        jmp     done

        align  64

term_17_loop:
        movq    mm3, [edi+esi]              ; get previous calculated value
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

        movq    mm2, [edi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    ; mm1 = zero
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pxor    mm5, mm0
        paddd   mm5, mm2                    ; and add to weight_AB
        pxor    mm5, mm0
        paddd   mm4, mm5                    ; add weights to sum
        dec     ecx
        jnz     term_17_loop

        mov     eax, [ebp+8]                ; access dpp
        movq    [eax+8], mm5                ; put weight_AB back
        movq    [eax+88], mm4               ; put sum_AB back
        emms
        jmp     term_1718_common_store

        align  64

term_18_loop:
        movq    mm3, [edi+esi]              ; get previous calculated value
        movq    mm0, mm3
        psubd   mm3, [edi+esi*2]
        psrad   mm3, 1
        paddd   mm3, mm0                    ; mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    ; mm1 = zero
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pxor    mm5, mm0
        paddd   mm5, mm2                    ; and add to weight_AB
        pxor    mm5, mm0
        dec     ecx
        paddd   mm4, mm5                    ; add weights to sum
        jnz     term_18_loop

        mov     eax, [ebp+8]                ; access dpp
        movq    [eax+8], mm5                ; put weight_AB back
        movq    [eax+88], mm4               ; put sum_AB back
        emms

term_1718_common_store:

        mov     eax, [ebp+8]                ; access dpp
        add     edi, esi                    ; back up a full sample
        mov     edx, [edi+4]                ; dpp->samples_B [0] = iptr [-1];
        mov     [eax+48], edx
        mov     edx, [edi]                  ; dpp->samples_A [0] = iptr [-2];
        mov     [eax+16], edx
        add     edi, esi                    ; back up another sample
        mov     edx, [edi+4]                ; dpp->samples_B [1] = iptr [-3];
        mov     [eax+52], edx
        mov     edx, [edi]                  ; dpp->samples_A [1] = iptr [-4];
        mov     [eax+20], edx
        jmp     done

        align  64

term_minus_1_loop:
        movq    mm3, [edi+esi]              ; mm3 = previous calculated value
        movq    mm2, [edi]                  ; mm2 = left_right
        psrlq   mm3, 32
        punpckldq mm3, mm2                  ; mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    ; mm1 = zero
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddd   mm5, mm1
        paddusw mm5, mm2                    ; and add to weight_AB
        psubd   mm5, mm1
        pxor    mm5, mm0
        paddd   mm4, mm5                    ; add weights to sum
        dec     ecx
        jnz     term_minus_1_loop

        mov     eax, [ebp+8]                ; access dpp
        movq    [eax+8], mm5                ; put weight_AB back
        movq    [eax+88], mm4               ; put sum_AB back
        emms

        add     edi, esi                    ; back up a full sample
        mov     edx, [edi+4]                ; dpp->samples_A [0] = iptr [-1];
        mov     eax, [ebp+8]
        mov     [eax+16], edx
        jmp     done

        align  64

term_minus_2_loop:
        movq    mm2, [edi]                  ; mm2 = left_right
        movq    mm3, mm2                    ; mm3 = swap dwords
        psrlq   mm3, 32
        punpckldq mm3, [edi+esi]            ; mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    ; mm1 = zero
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddd   mm5, mm1
        paddusw mm5, mm2                    ; and add to weight_AB
        psubd   mm5, mm1
        pxor    mm5, mm0
        paddd   mm4, mm5                    ; add weights to sum
        dec     ecx
        jnz     term_minus_2_loop

        mov     eax, [ebp+8]                ; access dpp
        movq    [eax+8], mm5                ; put weight_AB back
        movq    [eax+88], mm4               ; put sum_AB back
        emms

        add     edi, esi                    ; back up a full sample
        mov     edx, [edi]                  ; dpp->samples_B [0] = iptr [-2];
        mov     eax, [ebp+8]
        mov     [eax+48], edx
        jmp     done

        align  64

term_minus_3_loop:
        movq    mm0, [edi+esi]              ; mm0 = previous calculated value
        movq    mm3, mm0                    ; mm3 = swap dwords
        psrlq   mm3, 32
        punpckldq mm3, mm0                  ; mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [edi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [edi+ebx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     edi, esi
        pxor    mm1, mm1                    ; mm1 = zero
        pcmpeqd mm2, mm1                    ; mm2 = 1s if left_right was zero
        pcmpeqd mm3, mm1                    ; mm3 = 1s if sam_AB was zero
        por     mm2, mm3                    ; mm2 = 1s if either was zero
        pandn   mm2, mm6                    ; mask delta with zeros check
        pcmpeqd mm1, mm1
        psubd   mm1, mm7
        psubd   mm1, mm7
        psubd   mm1, mm0
        pxor    mm5, mm0
        paddd   mm5, mm1
        paddusw mm5, mm2                    ; and add to weight_AB
        psubd   mm5, mm1
        pxor    mm5, mm0
        paddd   mm4, mm5                    ; add weights to sum
        dec     ecx
        jnz     term_minus_3_loop

        mov     eax, [ebp+8]                ; access dpp
        movq    [eax+8], mm5                ; put weight_AB back
        movq    [eax+88], mm4               ; put sum_AB back
        emms

        add     edi, esi                    ; back up a full sample
        mov     edx, [edi+4]                ; dpp->samples_A [0] = iptr [-1];
        mov     eax, [ebp+8]
        mov     [eax+16], edx
        mov     edx, [edi]                  ; dpp->samples_B [0] = iptr [-2];
        mov     [eax+48], edx

done:   pop     edi
        pop     esi
        pop     ebx
        leave
        ret


; This is an assembly optimized version of the following WavPack function:
;
; void decorr_mono_buffer (int32_t *buffer,
;                          struct decorr_pass *decorr_passes,
;                          int32_t num_terms,
;                          int32_t sample_count)
;
; Decorrelate a buffer of mono samples, in place, as specified by the array
; of decorr_pass structures. Note that this function does NOT return the
; dpp->samples_X[] values in the "normalized" positions for terms 1-8, so if
; the number of samples is not a multiple of MAX_TERM, these must be moved if
; they are to be used somewhere else.
;
; IMPORTANT: There are two version of this function, but only one is enabled.
;
; The first is provided as a reference and takes arbitrary decorr_pass arrays
; with any combination of terms and deltas. The second is similar, but instead
; is hardcoded with the only four possible decorr_pass arrays that can be
; used here (see decorr_tables.h), and selects the appropriate one using
; only the "num_terms" parameter. This is significantly faster, especially
; in the "high" and "very high" modes. Note that there is no checking to make
; sure that the terms and deltas of the passed decorr_pass array match the
; hardcoded one, and that if they do not match then a very corrupt file
; will be generated. If in doubt, or when implementing for another processor,
; use the general purpose version.
;
; By using the overflow detection of the multiply instruction, these detect
; when the "long_math" varient is required.
;
; This is written to work on an IA-32 processor. The arguments are on the
; stack at these locations (after 5 pushes, we do not use ebp as a base
; pointer):
;
;   int32_t *buffer             [esp+24]
;   struct decorr_pass *dpp     [esp+28]
;   int32_t num_terms           [esp+32]
;   int32_t sample_count        [esp+36]
;
; register usage:
;
; ecx = sample being decorrelated
; esi = sample up counter
; edi = *buffer
; ebp = *dpp
;
; stack usage:
;
; [esp+0] = dpp end ptr
;

        if     0                            ; general-purpose version that handles all
                                            ; valid decorr_pass arrays - disable for
                                            ; hardcoded, faster in-line version

_pack_decorr_mono_buffer_x86:
        push    ebp                         ; save the resgister that we need to
        push    ebx
        push    esi
        push    edi
        push    eax                         ; this will be dpp end ptr

        mov     edx, [esp+32]               ; get number of terms
        imul    eax, edx, 96                ; calculate & store termination check ptr
        add     eax, [esp+28]
        mov     [esp], eax

        cmp     DWORD PTR [esp+36], 0       ; test & handle zero sample count & zero term count
        jz      mexit
        test    edx, edx
        jz      mexit

        mov     edi, [esp+24]
        mov     ebp, [esp+28]
        xor     esi, esi                     ; up counter = 0
        jmp     decorrelate_loop

        align  64

decorrelate_loop:
        mov     ecx, [edi+esi*4]             ; ecx is the sample we're decorrelating
nxterm: mov     edx, [ebp]
        cmp     dl, 17
        jge     @f

        mov     eax, esi
        and     eax, 7
        mov     ebx, [ebp+16+eax*4]
        add     eax, edx
        and     eax, 7
        mov     [ebp+16+eax*4], ecx
        jmp     domult

        align  4
@@:     mov     edx, [ebp+16]
        mov     [ebp+16], ecx
        je      @f
        lea     ebx, [edx+edx*2]
        sub     ebx, [ebp+20]
        sar     ebx, 1
        mov     [ebp+20], edx
        jmp     domult

        align  4
@@:     lea     ebx, [edx+edx]
        sub     ebx, [ebp+20]
        mov     [ebp+20], edx

domult: mov     eax, [ebp+8]
        mov     edx, eax
        imul    eax, ebx
        jo      multov                      ; on overflow, jump to use 64-bit imul varient
        sar     eax, 10
        sbb     ecx, eax
        je      @f
        test    ebx, ebx
        je      @f
        xor     ebx, ecx
        sar     ebx, 31
        xor     edx, ebx
        add     edx, [ebp+4]
        xor     edx, ebx
        mov     [ebp+8], edx
@@:     add     ebp, 96
        cmp     ebp, [esp]
        jnz     nxterm

        mov     [edi+esi*4], ecx            ; store completed sample
        mov     ebp, [esp+28]               ; reload decorr_passes pointer to first term
        inc     esi                         ; increment sample index
        cmp     esi, [esp+36]
        jnz     decorrelate_loop
        jmp     mexit

        align  4
multov: mov     eax, [ebp+8]
        imul    ebx
        shr     eax, 10
        sbb     ecx, eax
        shl     edx, 22
        sub     ecx, edx
        je      @f
        test    ebx, ebx
        je      @f
        xor     ebx, ecx
        sar     ebx, 31
        mov     eax, [ebp+8]
        xor     eax, ebx
        add     eax, [ebp+4]
        xor     eax, ebx
        mov     [ebp+8], eax
@@:     add     ebp, 96
        cmp     ebp, [esp]
        jnz     nxterm

        mov     [edi+esi*4], ecx            ; store completed sample
        mov     ebp, [esp+28]               ; reload decorr_passes pointer to first term
        inc     esi                         ; increment sample index
        cmp     esi, [esp+36]
        jnz     decorrelate_loop            ; loop all the way back this time

mexit:  pop     eax
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        else

; This version of pack_decorr_mono_buffer() is hardcoded for four specific decorr_pass arrays
; corresponding to the fast, normal, high, and very high modes. The terms of these filters
; are the first element in the tables defined in decorr_tables.h (with the negative terms
; replaced with 1). This macro processes the single specified term (with a fixed delta of 2)
; and updates the term pointer (ebp) with the specified offset when done. It assumes the
; following registers:
;
; ecx = sample being decorrelated
; esi = sample up counter (used for terms 1-8)
; ebp = decorr_pass pointer for this term (updated with "ebp_offset" when done)
; eax, ebx, edx = scratch
;

exeterm macro   term, ebp_offset
        local   over, cont, done

        if      term le 8
        mov     eax, esi
        and     eax, 7
        mov     ebx, [ebp+16+eax*4]
        if      term ne 8
        add     eax, term
        and     eax, 7
        endif
        mov     [ebp+16+eax*4], ecx

        elseif  term eq 17

        mov     edx, [ebp+16]               ; handle term 17
        mov     [ebp+16], ecx
        lea     ebx, [edx+edx]
        sub     ebx, [ebp+20]
        mov     [ebp+20], edx

        else

        mov     edx, [ebp+16]               ; handle term 18
        mov     [ebp+16], ecx
        lea     ebx, [edx+edx*2]
        sub     ebx, [ebp+20]
        sar     ebx, 1
        mov     [ebp+20], edx

        endif

        mov     eax, [ebp+8]
        imul    eax, ebx                    ; 32-bit multiply is almost always enough
        jo      over                        ; but handle overflow if it happens
        sar     eax, 10
        sbb     ecx, eax                    ; borrow flag provides rounding
        jmp     cont
over:   mov     eax, [ebp+8]                ; perform 64-bit multiply on overflow
        imul    ebx
        shr     eax, 10
        sbb     ecx, eax
        shl     edx, 22
        sub     ecx, edx
cont:   je      done
        test    ebx, ebx
        je      done
        xor     ebx, ecx
        sar     ebx, 30
        or      ebx, 1                      ; this generates delta of 1
        sal     ebx, 1                      ; this generates delta of 2
        add     [ebp+8], ebx
done:   add     ebp, ebp_offset

        endm

; This is the function entry point that uses the above macro for inline decorrelation. Only
; four different decorrelation filters are allowed and are hardcoded into this function.
;
; !!! NO CHECK PERFORMED TO MAKE SURE THE PASSED DECORR_PASS ARRAY MATCHES THE CODE !!!

_pack_decorr_mono_buffer_x86:
        push    ebp                         ; save the resgister that we need to
        push    ebx
        push    esi
        push    edi
        push    eax                         ; this will be sample count

        mov     eax, [esp+36]               ; load, test, & store sample count on stack
        and     eax, eax
        jz      @f
        mov     [esp], eax

        mov     edi, [esp+24]               ; edi = sample pointer, ebp = decorr_pass pointer
        mov     ebp, [esp+28]
        xor     esi, esi                    ; up counter = 0

        cmp     BYTE PTR [ebp], 18          ; for sanity check, make sure first term = 18, delta = 2
        jnz     @f
        cmp     BYTE PTR [ebp+4], 2
        jnz     @f

        mov     edx, [esp+32]               ; get number of terms
        cmp     dl, 2                       ; now use the term count to choose the loop
        jz      mono_fast_loop
        cmp     dl, 5
        jz      mono_normal_loop
        cmp     dl, 10
        jz      mono_high_loop
        cmp     dl, 16
        jz      mono_vhigh_loop

@@:     pop     eax                         ; pop sample count, saved regs, and return
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        align  64

mono_fast_loop:
        mov     ecx, [edi+esi*4]             ; ecx is the sample we're decorrelating

        exeterm 18,  96
        exeterm 17, -96

        mov     [edi+esi*4], ecx            ; store completed sample
        inc     esi                         ; increment sample index
        cmp     esi, [esp]
        jnz     mono_fast_loop              ; loop back for all samples

        pop     eax                         ; pop sample count, saved regs, and return
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        align  64

mono_normal_loop:
        mov     ecx, [edi+esi*4]             ; ecx is the sample we're decorrelating

        exeterm 18, 96
        exeterm 18, 96
        exeterm 2,  96
        exeterm 17, 96
        exeterm 3,  96*-4

        mov     [edi+esi*4], ecx            ; store completed sample
        inc     esi                         ; increment sample index
        cmp     esi, [esp]
        jnz     mono_normal_loop            ; loop back for all samples

        pop     eax                         ; pop sample count, saved regs, and return
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        align  64

mono_high_loop:
        mov     ecx, [edi+esi*4]             ; ecx is the sample we're decorrelating

        exeterm 18, 96
        exeterm 18, 96
        exeterm 18, 96
        exeterm 1,  96
        exeterm 2,  96
        exeterm 3,  96
        exeterm 5,  96
        exeterm 1,  96
        exeterm 17, 96
        exeterm 4,  96*-9

        mov     [edi+esi*4], ecx            ; store completed sample
        inc     esi                         ; increment sample index
        cmp     esi, [esp]
        jnz     mono_high_loop              ; loop back for all samples

        pop     eax                         ; pop sample count, saved regs, and return
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        align  64

mono_vhigh_loop:
        mov     ecx, [edi+esi*4]             ; ecx is the sample we're decorrelating

        exeterm 18, 96
        exeterm 18, 96
        exeterm 2,  96
        exeterm 3,  96
        exeterm 1,  96
        exeterm 18, 96
        exeterm 2,  96
        exeterm 4,  96
        exeterm 7,  96
        exeterm 5,  96
        exeterm 3,  96
        exeterm 6,  96
        exeterm 8,  96
        exeterm 1,  96
        exeterm 18, 96
        exeterm 2,  96*-15

        mov     [edi+esi*4], ecx            ; store completed sample
        inc     esi                         ; increment sample index
        cmp     esi, [esp]
        jnz     mono_vhigh_loop             ; loop back for all samples

        pop     eax                         ; pop sample count, saved regs, and return
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

        endif

; This is an assembly optimized version of the following WavPack function:
;
; uint32_t log2buffer (int32_t *samples, uint32_t num_samples, int limit);
;
; This function scans a buffer of 32-bit ints and accumulates the total
; log2 value of all the samples. This is useful for determining maximum
; compression because the bitstream storage required for entropy coding
; is proportional to the base 2 log of the samples.
;
; This is written to work on all IA-32 processors (i386, i486, etc.)
;
; No additional stack space is used; all storage is done in registers. The
; arguments on entry:
;
;   int32_t *samples            [ebp+8]
;   uint32_t num_samples        [ebp+12]
;   int limit                   [ebp+16]
;
; During the processing loops, the following registers are used:
;
;   esi             input buffer pointer
;   edi             sum accumulator
;   ebx             sample count
;   ebp             limit (if specified non-zero)
;   eax,ecx,edx     scratch
;

        align  256
        .radix 16

log2_table:
        byte   000, 001, 003, 004, 006, 007, 009, 00a, 00b, 00d, 00e, 010, 011, 012, 014, 015
        byte   016, 018, 019, 01a, 01c, 01d, 01e, 020, 021, 022, 024, 025, 026, 028, 029, 02a
        byte   02c, 02d, 02e, 02f, 031, 032, 033, 034, 036, 037, 038, 039, 03b, 03c, 03d, 03e
        byte   03f, 041, 042, 043, 044, 045, 047, 048, 049, 04a, 04b, 04d, 04e, 04f, 050, 051
        byte   052, 054, 055, 056, 057, 058, 059, 05a, 05c, 05d, 05e, 05f, 060, 061, 062, 063
        byte   064, 066, 067, 068, 069, 06a, 06b, 06c, 06d, 06e, 06f, 070, 071, 072, 074, 075
        byte   076, 077, 078, 079, 07a, 07b, 07c, 07d, 07e, 07f, 080, 081, 082, 083, 084, 085
        byte   086, 087, 088, 089, 08a, 08b, 08c, 08d, 08e, 08f, 090, 091, 092, 093, 094, 095
        byte   096, 097, 098, 099, 09a, 09b, 09b, 09c, 09d, 09e, 09f, 0a0, 0a1, 0a2, 0a3, 0a4
        byte   0a5, 0a6, 0a7, 0a8, 0a9, 0a9, 0aa, 0ab, 0ac, 0ad, 0ae, 0af, 0b0, 0b1, 0b2, 0b2
        byte   0b3, 0b4, 0b5, 0b6, 0b7, 0b8, 0b9, 0b9, 0ba, 0bb, 0bc, 0bd, 0be, 0bf, 0c0, 0c0
        byte   0c1, 0c2, 0c3, 0c4, 0c5, 0c6, 0c6, 0c7, 0c8, 0c9, 0ca, 0cb, 0cb, 0cc, 0cd, 0ce
        byte   0cf, 0d0, 0d0, 0d1, 0d2, 0d3, 0d4, 0d4, 0d5, 0d6, 0d7, 0d8, 0d8, 0d9, 0da, 0db
        byte   0dc, 0dc, 0dd, 0de, 0df, 0e0, 0e0, 0e1, 0e2, 0e3, 0e4, 0e4, 0e5, 0e6, 0e7, 0e7
        byte   0e8, 0e9, 0ea, 0ea, 0eb, 0ec, 0ed, 0ee, 0ee, 0ef, 0f0, 0f1, 0f1, 0f2, 0f3, 0f4
        byte   0f4, 0f5, 0f6, 0f7, 0f7, 0f8, 0f9, 0f9, 0fa, 0fb, 0fc, 0fc, 0fd, 0fe, 0ff, 0ff

        .radix  10

_log2buffer_x86:
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        cld

        mov     esi, [ebp+8]                ; esi = sample source pointer
        xor     edi, edi                    ; edi = 0 (accumulator)
        mov     ebx, [ebp+12]               ; ebx = num_samples
        test    ebx, ebx                    ; exit now if none, sum = 0
        jz      normal_exit

        mov     ebp, [ebp+16]               ; ebp = limit
        test    ebp, ebp                    ; we have separate loops for limit and no limit
        jz      no_limit_loop
        jmp     limit_loop

        align  64

limit_loop:
        mov     eax, [esi]                  ; get next sample into eax
        cdq                                 ; edx = sign of sample (for abs)
        add     esi, 4
        xor     eax, edx
        sub     eax, edx
        je      L40                         ; skip if sample was zero
        mov     edx, eax                    ; move to edx and apply rounding
        shr     eax, 9
        add     edx, eax
        bsr     ecx, edx                    ; ecx = MSB set in sample (0 - 31)
        lea     eax, [ecx+1]                ; eax = number used bits in sample (1 - 32)
        sub     ecx, 8                      ; ecx = shift right amount (-8 to 23)
        ror     edx, cl                     ; use rotate to do "signed" shift 
        sal     eax, 8                      ; move nbits to integer portion of log
        movzx   edx, dl                     ; dl = mantissa, look up log fraction in table 
        mov     al, BYTE PTR [log2_table+edx] ; eax = combined integer and fraction for full log
        add     edi, eax                    ; add to running sum and compare to limit
        cmp     eax, ebp
        jge     limit_exceeded
L40:    sub     ebx, 1                      ; loop back if more samples
        jne     limit_loop
        jmp     normal_exit

        align  64

no_limit_loop:
        mov     eax, [esi]                  ; get next sample into eax
        cdq                                 ; edx = sign of sample (for abs)
        add     esi, 4
        xor     eax, edx
        sub     eax, edx
        je      L45                         ; skip if sample was zero
        mov     edx, eax                    ; move to edx and apply rounding
        shr     eax, 9
        add     edx, eax
        bsr     ecx, edx                    ; ecx = MSB set in sample (0 - 31)
        lea     eax, [ecx+1]                ; eax = number used bits in sample (1 - 32)
        sub     ecx, 8                      ; ecx = shift right amount (-8 to 23)
        ror     edx, cl                     ; use rotate to do "signed" shift 
        sal     eax, 8                      ; move nbits to integer portion of log
        movzx   edx, dl                     ; dl = mantissa, look up log fraction in table 
        mov     al, BYTE PTR [log2_table+edx] ; eax = combined integer and fraction for full log
        add     edi, eax                    ; add to running sum
L45:    sub     ebx, 1                      ; loop back if more samples
        jne     no_limit_loop
        jmp     normal_exit

limit_exceeded:
        mov     edi, -1                     ; -1 return means log limit exceeded
normal_exit:
        mov     eax, edi                    ; move sum accumulator into eax for return
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

; Helper function to determine if specified CPU feature is available (used here for MMX).
; Input parameter is index of feature to be checked (EDX from CPUID(1) only, MMX = 23).
; Return value is the specified bit (0 or 1) or 0 if CPUID is not supported.

_pack_cpu_has_feature_x86:
        pushfd                              ; save eflags
        pushfd                              ; push another copy
        xor     dword ptr [esp], 200000h    ; toggle ID bit on stack & pop it back into eflags
        popfd
        pushfd                              ; store possibly modified eflags
        pop     eax                         ; and pop back into eax
        xor     eax, [esp]                  ; compare to original pushed eflags
        popfd                               ; restore original eflags
        and     eax, 200000h                ; eax = 1 if eflags ID bit was changable
        jz      oldcpu                      ; return zero if CPUID is not available (wow!)

        push    ebx                         ; we must save ebx
        mov     eax, 1                      ; do cpuid (1) to get features into edx
        cpuid
        mov     eax, edx                    ; copy into eax for shift
        mov     cl, [esp+8]                 ; get parameter and shift that bit index into LSB
        sar     eax, cl
        and     eax, 1
        pop     ebx                         ; restore ebx and return 0 or 1

oldcpu: ret                                 ; return value in eax

asmcode ends

        end

