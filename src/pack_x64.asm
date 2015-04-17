;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                           **** WAVPACK ****                            ;;
;;                  Hybrid Lossless Wavefile Compressor                   ;;
;;              Copyright (c) 1998 - 2015 Conifer Software.               ;;
;;                          All Rights Reserved.                          ;;
;;      Distributed under the BSD Software License (see license.txt)      ;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

        include <ksamd64.inc>

        public  pack_decorr_stereo_pass_cont_rev_x64win
        public  pack_decorr_stereo_pass_cont_x64win

asmcode segment page 'CODE'

; This module contains X64 assembly optimized versions of functions required
; to encode WavPack files.

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
; This is written to work on an X86-64 processor (also called the AMD64)
; running in 64-bit mode and uses the MMX extensions to improve the
; performance by processing both stereo channels together. It is based on
; the original MMX code written by Joachim Henke that used MMX intrinsics
; called from C. Many thanks to Joachim for that!
;
; This version is for 64-bit Windows. Note that the two public functions
; are "leaf" functions that simply load rax with the direction and jump
; into the private common "frame" function. The arguments are passed in
; registers:
;
;   struct decorr_pass *dpp     rcx
;   int32_t *in_buffer          rdx
;   int32_t *out_buffer         r8
;   int32_t sample_count        r9d
;
; During the processing loops, the following registers are used:
;
;   rdi         input buffer pointer
;   rsi         direction (-8 forward, +8 reverse)
;   rbx         delta from input to output buffer
;   ecx         sample count
;   rdx         sign (dir) * term * -8 (terms 1-8 only)
;   mm0, mm1    scratch
;   mm2         original sample values
;   mm3         correlation samples
;   mm4         weight sums
;   mm5         weights
;   mm6         delta
;   mm7         512 (for rounding)
;
; stack usage:
;
; [rsp+0] = *dpp
;

pack_decorr_stereo_pass_cont_rev_x64win:
        mov     rax, 8                      ; get value for reverse direction & jump
        jmp     pack_decorr_stereo_pass_cont_common

pack_decorr_stereo_pass_cont_x64win:
        mov     rax, -8                     ; get value for forward direction & jump
        jmp     pack_decorr_stereo_pass_cont_common

pack_decorr_stereo_pass_cont_common proc frame
        push_reg    rbp                     ; save non-volatile registers on stack
        push_reg    rbx                     ; (alphabetically)
        push_reg    rdi
        push_reg    rsi
        alloc_stack 8                       ; allocate 8 bytes on stack & align to 16 bytes
        end_prologue

        mov     [rsp], rcx                  ; [rsp] = *dpp
        mov     rdi, rcx                    ; copy params from win regs to Linux regs
        mov     rsi, rdx                    ; so we can leave following code similar
        mov     rdx, r8
        mov     rcx, r9

        mov     rdi, rsi                    ; rdi = inbuffer
        mov     rsi, rax                    ; rsi = -direction

        mov     eax, 512
        movd    mm7, eax
        punpckldq mm7, mm7                  ; mm7 = round (512)

        mov     rax, [rsp]                  ; access dpp
        mov     eax, [rax+4]
        movd    mm6, eax
        punpckldq mm6, mm6                  ; mm6 = delta (0-7)

        mov     rax, [rsp]                  ; access dpp
        movq    mm5, [rax+8]                ; mm5 = weight_AB
        movq    mm4, [rax+88]               ; mm4 = sum_AB

        mov     rbx, rdx                    ; rbx = out_buffer (rdx) - in_buffer (rdi)
        sub     rbx, rdi

        mov     rax, [rsp]                  ; *eax = dpp
        movsxd  rax, DWORD PTR [rax]        ; get term and vector to correct loop
        cmp     al, 17
        je      term_17_loop
        cmp     al, 18
        je      term_18_loop
        cmp     al, -1
        je      term_minus_1_loop
        cmp     al, -2
        je      term_minus_2_loop
        cmp     al, -3
        je      term_minus_3_loop

        sal     rax, 3
        mov     rdx, rax                    ; rdx = term * 8 to index correlation sample
        test    rsi, rsi                    ; test direction
        jns     default_term_loop
        neg     rdx
        jmp     default_term_loop

        align  64

default_term_loop:
        movq    mm3, [rdi+rdx]              ; mm3 = sam_AB

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [rdi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [rdi+rbx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     rdi, rsi
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

        mov     rax, [rsp]                  ; access dpp
        movq    [rax+8], mm5                ; put weight_AB back
        movq    [rax+88], mm4               ; put sum_AB back
        emms

        mov     rdx, [rsp]                  ; access dpp with rdx
        movsxd  rcx, DWORD PTR [rdx]        ; rcx = dpp->term

default_store_samples:
        dec     rcx
        add     rdi, rsi                    ; back up one full sample
        mov     eax, [rdi+4]
        mov     [rdx+rcx*4+48], eax         ; store samples_B [ecx]
        mov     eax, [rdi]
        mov     [rdx+rcx*4+16], eax         ; store samples_A [ecx]
        test    rcx, rcx
        jnz     default_store_samples
        jmp     done

        align  64

term_17_loop:
        movq    mm3, [rdi+rsi]              ; get previous calculated value
        paddd   mm3, mm3
        psubd   mm3, [rdi+rsi*2]

        movq    mm1, mm3
        pslld   mm1, 17
        psrld   mm1, 17
        pmaddwd mm1, mm5

        movq    mm0, mm3
        pslld   mm0, 1
        psrld   mm0, 16
        pmaddwd mm0, mm5

        movq    mm2, [rdi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [rdi+rbx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     rdi, rsi
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

        mov     rax, [rsp]                  ; access dpp
        movq    [rax+8], mm5                ; put weight_AB back
        movq    [rax+88], mm4               ; put sum_AB back
        emms
        jmp     term_1718_common_store

        align  64

term_18_loop:
        movq    mm3, [rdi+rsi]              ; get previous calculated value
        movq    mm0, mm3
        psubd   mm3, [rdi+rsi*2]
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

        movq    mm2, [rdi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [rdi+rbx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     rdi, rsi
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

        mov     rax, [rsp]                  ; access dpp
        movq    [rax+8], mm5                ; put weight_AB back
        movq    [rax+88], mm4               ; put sum_AB back
        emms

term_1718_common_store:

        mov     rax, [rsp]                  ; access dpp
        add     rdi, rsi                    ; back up a full sample
        mov     edx, [rdi+4]                ; dpp->samples_B [0] = iptr [-1];
        mov     [rax+48], edx
        mov     edx, [rdi]                  ; dpp->samples_A [0] = iptr [-2];
        mov     [rax+16], edx
        add     rdi, rsi                    ; back up another sample
        mov     edx, [rdi+4]                ; dpp->samples_B [1] = iptr [-3];
        mov     [rax+52], edx
        mov     edx, [rdi]                  ; dpp->samples_A [1] = iptr [-4];
        mov     [rax+20], edx
        jmp     done

        align  64

term_minus_1_loop:
        movq    mm3, [rdi+rsi]              ; mm3 = previous calculated value
        movq    mm2, [rdi]                  ; mm2 = left_right
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
        movq    [rdi+rbx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     rdi, rsi
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

        mov     rax, [rsp]                  ; access dpp
        movq    [rax+8], mm5                ; put weight_AB back
        movq    [rax+88], mm4               ; put sum_AB back
        emms

        add     rdi, rsi                    ; back up a full sample
        mov     edx, [rdi+4]                ; dpp->samples_A [0] = iptr [-1];
        mov     rax, [rsp]
        mov     [rax+16], edx
        jmp     done

        align  64

term_minus_2_loop:
        movq    mm2, [rdi]                  ; mm2 = left_right
        movq    mm3, mm2                    ; mm3 = swap dwords
        psrlq   mm3, 32
        punpckldq mm3, [rdi+rsi]            ; mm3 = sam_AB

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
        movq    [rdi+rbx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     rdi, rsi
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

        mov     rax, [rsp]                  ; access dpp
        movq    [rax+8], mm5                ; put weight_AB back
        movq    [rax+88], mm4               ; put sum_AB back
        emms

        add     rdi, rsi                    ; back up a full sample
        mov     edx, [rdi]                  ; dpp->samples_B [0] = iptr [-2];
        mov     rax, [rsp]
        mov     [rax+48], edx
        jmp     done

        align  64

term_minus_3_loop:
        movq    mm0, [rdi+rsi]              ; mm0 = previous calculated value
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

        movq    mm2, [rdi]                  ; mm2 = left_right
        pslld   mm0, 5
        paddd   mm1, mm7                    ; add 512 for rounding
        psrad   mm1, 10
        psubd   mm2, mm0
        psubd   mm2, mm1                    ; add shifted sums
        movq    mm0, mm3
        movq    [rdi+rbx], mm2              ; store result
        pxor    mm0, mm2
        psrad   mm0, 31                     ; mm0 = sign (sam_AB ^ left_right)
        sub     rdi, rsi
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

        mov     rax, [rsp]                  ; access dpp
        movq    [rax+8], mm5                ; put weight_AB back
        movq    [rax+88], mm4               ; put sum_AB back
        emms

        add     rdi, rsi                    ; back up a full sample
        mov     edx, [rdi+4]                ; dpp->samples_A [0] = iptr [-1];
        mov     rax, [rsp]
        mov     [rax+16], edx
        mov     edx, [rdi]                  ; dpp->samples_B [0] = iptr [-2];
        mov     [rax+48], edx

done:   add     rsp, 8                      ; begin epilog by deallocating stack
        pop     rsi                         ; restore non-volatile registers & return
        pop     rdi
        pop     rbx
        pop     rbp
        ret

pack_decorr_stereo_pass_cont_common endp

; This is an assembly optimized version of the following WavPack function:
;
; void decorr_mono_pass_cont (int32_t *out_buffer,
;                             int32_t *in_buffer,
;                             struct decorr_pass *dpp,
;                             int32_t sample_count);
;
; It performs a single pass of mono decorrelation, transfering from the
; input buffer to the output buffer. Note that this version of the function
; requires that the up to 8 previous (depending on dpp->term) mono samples
; are visible and correct. In other words, it ignores the "samples_*"
; fields in the decorr_pass structure and gets the history data directly
; from the source buffer. It does, however, return the appropriate history
; samples to the decorr_pass structure before returning.
;
; By using the overflow detection of the multiply instruction, it detects
; when the "long_math" varient is required and automatically branches to it
; for the rest of the loop.
;
; This is written to work on an X86-64 processor (also called the AMD64)
; running in 64-bit mode. This version is for 64-bit Windows and the
; arguments are passed in registers:
;
;   int32_t *out_buffer         rcx
;   int32_t *in_buffer          rdx
;   struct decorr_pass *dpp     r8
;   int32_t sample_count        r9
;
; stack usage:
;
; [rsp] = *dpp
;
; Register usage:
;
; rsi = source ptr
; rdi = destination ptr
; rcx = term * -4 (default terms)
; rcx = previous sample (terms 17 & 18)
; ebp = weight
; r8d = delta
; r9  = eptr
;

pack_decorr_mono_pass_cont_x64win proc public frame
        push_reg    rbp                     ; save non-volatile registers on stack
        push_reg    rbx                     ; (alphabetically)
        push_reg    rdi
        push_reg    rsi
        alloc_stack 8                       ; allocate 8 bytes on stack & align to 16 bytes
        end_prologue

        mov     [rsp], r8                   ; [rsp] = *dpp
        mov     rdi, rcx                    ; copy params from win regs to Linux regs
        mov     rsi, rdx                    ; so we can leave following code similar
        mov     rdx, r8
        mov     rcx, r9

        test    ecx, ecx                    ; test & handle zero sample count
        jz      mono_done

        cld
        mov     r8d, [rdx+4]                ; rd8 = delta
        mov     ebp, [rdx+8]                ; ebp = weight
        lea     r9, [rsi+rcx*4]             ; r9 = eptr
        mov     ecx, [rsi-4]                ; preload last sample
        mov     eax, [rdx]                  ; get term
        cmp     al, 17
        je      mono_term_17_loop
        cmp     al, 18
        je      mono_term_18_loop

        imul    rcx, rax, -4                ; rcx is index to correlation sample
        jmp     mono_default_term_loop

        align  64

mono_default_term_loop:
        mov     edx, [rsi+rcx]
        mov     ebx, edx
        imul    edx, ebp
        jo      mono_default_term_long      ; overflow pops us into long_math version
        lodsd
        sar     edx, 10
        sbb     eax, edx
        stosd
        je      S280
        test    ebx, ebx
        je      S280
        xor     eax, ebx
        cdq
        xor     ebp, edx
        add     ebp, r8d
        xor     ebp, edx
S280:   cmp     rsi, r9
        jnz     mono_default_term_loop
        jmp     mono_default_term_done

        align  64

mono_default_term_long:
        mov     eax, [rsi+rcx]
        mov     ebx, eax
        imul    ebp
        shl     edx, 22
        shr     eax, 10
        adc     edx, eax                    ; edx = apply_weight (sam_A)
        lodsd
        sub     eax, edx
        stosd
        je      L280
        test    ebx, ebx
        je      L280
        xor     eax, ebx
        cdq
        xor     ebp, edx
        add     ebp, r8d
        xor     ebp, edx
L280:   cmp     rsi, r9
        jnz     mono_default_term_long

mono_default_term_done:
        mov     rdx, [rsp]                  ; rdx = *dpp
        mov     [rdx+8], ebp                ; put weight back
        movsxd  rcx, DWORD PTR [rdx]        ; rcx = dpp->term

mono_default_store_samples:
        dec     rcx
        sub     rsi, 4                      ; back up one sample
        mov     eax, [rsi]
        mov     [rdx+rcx*4+16], eax         ; store samples_A [ecx]
        test    rcx, rcx
        jnz     mono_default_store_samples
        jmp     mono_done

        align  64

mono_term_17_loop:
        lea     edx, [rcx+rcx]
        sub     edx, [rsi-8]                ; ebx = sam_A
        mov     ebx, edx
        imul    edx, ebp
        jo      mono_term_17_long           ; overflow pops us into long_math version
        sar     edx, 10
        lodsd
        mov     ecx, eax
        sbb     eax, edx
        stosd
        je      S282
        test    ebx, ebx
        je      S282
        xor     eax, ebx
        cdq
        xor     ebp, edx
        add     ebp, r8d
        xor     ebp, edx
S282:   cmp     rsi, r9
        jnz     mono_term_17_loop
        jmp     mono_term_1718_exit

        align  64

mono_term_17_long:
        lea     eax, [rcx+rcx]
        sub     eax, [rsi-8]                ; ebx = sam_A
        mov     ebx, eax
        imul    ebp
        shl     edx, 22
        shr     eax, 10
        adc     edx, eax
        lodsd
        mov     ecx, eax
        sub     eax, edx
        stosd
        je      L282
        test    ebx, ebx
        je      L282
        xor     eax, ebx
        cdq
        xor     ebp, edx
        add     ebp, r8d
        xor     ebp, edx
L282:   cmp     rsi, r9
        jnz     mono_term_17_long
        jmp     mono_term_1718_exit

        align  64

mono_term_18_loop:
        lea     edx, [rcx+rcx*2]
        sub     edx, [rsi-8]
        sar     edx, 1
        mov     ebx, edx                    ; ebx = sam_A
        imul    edx, ebp
        jo      mono_term_18_long           ; overflow pops us into long_math version
        sar     edx, 10
        lodsd
        mov     ecx, eax
        sbb     eax, edx
        stosd
        je      S283
        test    ebx, ebx
        je      S283
        xor     eax, ebx
        cdq
        xor     ebp, edx
        add     ebp, r8d
        xor     ebp, edx
S283:   cmp     rsi, r9
        jnz     mono_term_18_loop
        jmp     mono_term_1718_exit

        align  64

mono_term_18_long:
        lea     eax, [rcx+rcx*2]
        sub     eax, [rsi-8]
        sar     eax, 1
        mov     ebx, eax                    ; ebx = sam_A
        imul    ebp
        shl     edx, 22
        shr     eax, 10
        adc     edx, eax
        lodsd
        mov     ecx, eax
        sub     eax, edx
        stosd
        je      L283
        test    ebx, ebx
        je      L283
        xor     eax, ebx
        cdq
        xor     ebp, edx
        add     ebp, r8d
        xor     ebp, edx
L283:   cmp     rsi, r9
        jnz     mono_term_18_long

mono_term_1718_exit:
        mov     rdx, [rsp]                  ; rdx = *dpp
        mov     [rdx+8], ebp                ; put weight back
        mov     eax, [rsi-4]                ; dpp->samples_A [0] = bptr [-1]
        mov     [rdx+16], eax
        mov     eax, [rsi-8]                ; dpp->samples_A [1] = bptr [-2]
        mov     [rdx+20], eax

mono_done:
        add     rsp, 8                      ; begin epilog by deallocating stack
        pop     rsi                         ; restore non-volatile registers & return
        pop     rdi
        pop     rbx
        pop     rbp
        ret

pack_decorr_mono_pass_cont_x64win endp

; This is an assembly optimized version of the following WavPack function:
;
; uint32_t log2buffer (int32_t *samples, uint32_t num_samples, int limit);
;
; This function scans a buffer of 32-bit ints and accumulates the total
; log2 value of all the samples. This is useful for determining maximum
; compression because the bitstream storage required for entropy coding
; is proportional to the base 2 log of the samples.
;
; This is written to work on an X86-64 processor (also called the AMD64)
; running in 64-bit mode. This version is for 64-bit Windows and the
; arguments are passed in registers:
;
;   int32_t *samples            rcx
;   uint32_t num_samples        rdx
;   int limit                   r8
;
; During the processing loops, the following registers are used:
;
;   r8              pointer to the 256-byte log fraction table
;   rsi             input buffer pointer
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

log2buffer_x64win proc public frame
        push_reg    rbp                     ; save non-volatile registers on stack
        push_reg    rbx                     ; (alphabetically)
        push_reg    rdi
        push_reg    rsi
        alloc_stack 8                       ; allocate 8 bytes on stack & align to 16 bytes
        end_prologue

        mov     rdi, rcx                    ; copy params from win regs to Linux regs
        mov     rsi, rdx                    ; so we can leave following code similar
        mov     rdx, r8

        mov     ebx, esi                    ; ebx = num_samples
        mov     rsi, rdi                    ; rsi = *samples
        xor     edi, edi                    ; initialize sum
        lea     r8, log2_table
        test    ebx, ebx                    ; test count for zero
        jz      normal_exit
        mov     ebp, edx                    ; ebp = limit
        test    ebp, ebp                    ; we have separate loops for limit and no limit
        jz      no_limit_loop
        jmp     limit_loop

        align  64

limit_loop:
        mov     eax, [rsi]                  ; get next sample into eax
        cdq                                 ; edx = sign of sample (for abs)
        add     rsi, 4
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
        mov     al, [r8+rdx]                ; eax = combined integer and fraction for full log
        add     edi, eax                    ; add to running sum and compare to limit
        cmp     eax, ebp
        jge     limit_exceeded
L40:    sub     ebx, 1                      ; loop back if more samples
        jne     limit_loop
        jmp     normal_exit

        align  64

no_limit_loop:
        mov     eax, [rsi]                  ; get next sample into eax
        cdq                                 ; edx = sign of sample (for abs)
        add     rsi, 4
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
        mov     al, [r8+rdx]                ; eax = combined integer and fraction for full log
        add     edi, eax                    ; add to running sum
L45:    sub     ebx, 1
        jne     no_limit_loop
        jmp     normal_exit

limit_exceeded:
        mov     edi, -1                     ; return -1 to indicate limit hit
normal_exit:
        mov     eax, edi                    ; move sum accumulator into eax for return

        add     rsp, 8                      ; begin epilog by deallocating stack
        pop     rsi                         ; restore non-volatile registers & return
        pop     rdi
        pop     rbx
        pop     rbp
        ret

log2buffer_x64win endp

asmcode ends

        end

