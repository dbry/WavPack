////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2015 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

        .text
        .align
        .global         unpack_decorr_stereo_pass_cont_armv7
        .global         unpack_decorr_mono_pass_cont_armv7

/* This is an assembly optimized version of the following WavPack function:
 *
 * void decorr_stereo_pass_cont (struct decorr_pass *dpp,
 *                               int32_t *buffer,
 *                               int32_t sample_counti,
 *                               int32_t long_math);
 *
 * It performs a single pass of stereo decorrelation on the provided buffer.
 * Note that this version of the function requires that up to 8 previous stereo
 * samples are visible and correct. In other words, it ignores the "samples_*"
 * fields in the decorr_pass structure and gets the history data directly
 * from the buffer. It does, however, return the appropriate history samples
 * to the decorr_pass structure before returning.
 *
 * This is written to work on a ARM7TDMI processor. Based on the "long_math"
 * parameter, this code selects between loop versions that use the 32-bit
 * multiply-accumulate instruction (and so will overflow with 24-bit
 * WavPack files) and loop versions that use the 64-bit multiply-accumulate
 * instruction (which can be slower).
 *
 * A mono version follows below. 
 */

/*
 * on entry:
 *
 * r0 = struct decorr_pass *dpp
 * r1 = int32_t *buffer
 * r2 = int32_t sample_count
 * r3 = int32_t long_math
 */

unpack_decorr_stereo_pass_cont_armv7:

        stmfd   sp!, {r4 - r8, r10, r11, lr}
        cmp     r3, #0                  @ check for long versions required
        bne     long_versions           @ branch if yes

        mov     r5, r0                  @ r5 = dpp
        mov     r11, #512               @ r11 = 512 for rounding
        ldr     r6, [r0, #4]            @ r6 = dpp->delta
        ldr     r4, [r0, #8]            @ r4 = dpp->weight_A
        ldr     r0, [r0, #12]           @ r0 = dpp->weight_B
        cmp     r2, #0                  @ exit if no samples to process
        beq     common_exit

        add     r7, r1, r2, asl #3      @ r7 = buffer ending position
        ldr     r2, [r5, #0]            @ r2 = dpp->term
        cmp     r2, #0
        bmi     minus_term

        ldr     lr, [r1, #-16]          @ load 2 sample history from buffer
        ldr     r10, [r1, #-12]         @  for terms 2, 17, and 18
        ldr     r8, [r1, #-8]
        ldr     r3, [r1, #-4]
        cmp     r2, #17
        beq     term_17_loop
        cmp     r2, #18
        beq     term_18_loop
        cmp     r2, #2
        beq     term_2_loop
        b       term_default_loop       @ else handle default (1-8, except 2)

minus_term:
        mov     r10, #1024              @ r10 = -1024 for weight clipping
        rsb     r10, r10, #0            @  (only used for negative terms)
        cmn     r2, #1
        beq     term_minus_1
        cmn     r2, #2
        beq     term_minus_2
        cmn     r2, #3
        beq     term_minus_3
        b       common_exit

/*
 ******************************************************************************
 * Loop to handle term = 17 condition
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 = second previous right sample
 * r3 = previous right sample   r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = current decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous left sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

term_17_loop:
        rsbs    ip, lr, r8, asl #1      @ decorr value = (2 * prev) - 2nd prev
        mov     lr, r8                  @ previous becomes 2nd previous
        ldr     r2, [r1], #4            @ get sample & update pointer
        mla     r8, ip, r4, r11         @ mult decorr value by weight, round,
        add     r8, r2, r8, asr #10     @  shift, and add to new sample
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     S325
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S325:   rsbs    ip, r10, r3, asl #1     @ do same thing for right channel
        mov     r10, r3
        ldr     r2, [r1], #4
        mla     r3, ip, r0, r11
        add     r3, r2, r3, asr #10
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     S329
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6

S329:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     term_17_loop
        b       store_1718              @ common exit for terms 17 & 18

/*
 ******************************************************************************
 * Loop to handle term = 18 condition
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 = second previous right sample
 * r3 = previous right sample   r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous left sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

term_18_loop:
        sub     ip, r8, lr              @ decorr value =
        mov     lr, r8                  @  ((3 * prev) - 2nd prev) >> 1
        adds    ip, r8, ip, asr #1
        ldr     r2, [r1], #4            @ get sample & update pointer
        mla     r8, ip, r4, r11         @ mult decorr value by weight, round,
        add     r8, r2, r8, asr #10     @  shift, and add to new sample
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     S337
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S337:   sub     ip, r3, r10             @ do same thing for right channel
        mov     r10, r3
        adds    ip, r3, ip, asr #1
        ldr     r2, [r1], #4
        mla     r3, ip, r0, r11
        add     r3, r2, r3, asr #10
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     S341
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6

S341:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     term_18_loop

/* common exit for terms 17 & 18 */

store_1718:
        str     r3, [r5, #48]           @ store sample history into struct
        str     r8, [r5, #16]
        str     r10, [r5, #52]
        str     lr, [r5, #20]
        b       common_exit             @ and return

/*
 ******************************************************************************
 * Loop to handle term = 2 condition
 * (note that this case can be handled by the default term handler (1-8), but
 * this special case is faster because it doesn't have to read memory twice)
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 = second previous right sample
 * r3 = previous right sample   r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous left sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

term_2_loop:
        movs    ip, lr                  @ get decorrelation value & test
        mov     lr, r8                  @ previous becomes 2nd previous
        ldr     r2, [r1], #4            @ get sample & update pointer
        mla     r8, ip, r4, r11         @ mult decorr value by weight, round,
        add     r8, r2, r8, asr #10     @  shift, and add to new sample
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     S225
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S225:   movs    ip, r10                 @ do same thing for right channel
        mov     r10, r3
        ldr     r2, [r1], #4
        mla     r3, ip, r0, r11
        add     r3, r2, r3, asr #10
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     S229
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6

S229:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     term_2_loop
        b       default_term_exit       @ this exit updates all dpp->samples

/*
 ******************************************************************************
 * Loop to handle default term condition
 *
 * r0 = dpp->weight_B           r8 = result accumulator
 * r1 = bptr                    r9 =
 * r2 = dpp->term               r10 =
 * r3 = decorrelation value     r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr =
 * r7 = eptr                    pc =
 *******************************************************************************
 */

term_default_loop:
        ldr     ip, [r1]                @ get original sample
        ldr     r3, [r1, -r2, asl #3]   @ get decorrelation value based on term
        mla     r8, r3, r4, r11         @ mult decorr value by weight, round,
        add     r8, ip, r8, asr #10     @  shift and add to new sample
        str     r8, [r1], #4            @ store update sample
        cmp     r3, #0
        cmpne   ip, #0
        beq     S350
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S350:   ldr     ip, [r1]                @ do the same thing for right channel
        ldr     r3, [r1, -r2, asl #3]
        mla     r8, r3, r0, r11
        add     r8, ip, r8, asr #10
        str     r8, [r1], #4
        cmp     r3, #0
        cmpne   ip, #0
        beq     S354
        teq     ip, r3
        submi   r0, r0, r6
        addpl   r0, r0, r6

S354:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     term_default_loop

/*
 * This exit is used by terms 1-8 to store the previous "term" samples (up to 8)
 * into the decorr pass structure history
 */

default_term_exit:
        ldr     r2, [r5, #0]            @ r2 = dpp->term

S358:   sub     r2, r2, #1
        sub     r1, r1, #8
        ldr     r3, [r1, #4]            @ get right sample and store in dpp->samples_B [r2]
        add     r6, r5, #48
        str     r3, [r6, r2, asl #2]
        ldr     r3, [r1, #0]            @ get left sample and store in dpp->samples_A [r2]
        add     r6, r5, #16
        str     r3, [r6, r2, asl #2]
        cmp     r2, #0
        bne     S358
        b       common_exit

/*
 ******************************************************************************
 * Loop to handle term = -1 condition
 *
 * r0 = dpp->weight_B           r8 =
 * r1 = bptr                    r9 =
 * r2 = intermediate result     r10 = -1024 (for clipping)
 * r3 = previous right sample   r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = updated left sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

term_minus_1:
        ldr     r3, [r1, #-4]

term_minus_1_loop:
        ldr     ip, [r1]                @ for left channel the decorrelation value
        mla     r2, r3, r4, r11         @  is the previous right sample (in r3)
        add     lr, ip, r2, asr #10
        str     lr, [r1], #8
        cmp     r3, #0
        cmpne   ip, #0
        beq     S361
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6
        cmp     r4, #1024
        movgt   r4, #1024
        cmp     r4, r10
        movlt   r4, r10

S361:   ldr     r2, [r1, #-4]           @ for right channel the decorrelation value
        mla     r3, lr, r0, r11         @  is the just updated right sample (in lr)
        add     r3, r2, r3, asr #10
        str     r3, [r1, #-4]
        cmp     lr, #0
        cmpne   r2, #0
        beq     S369
        teq     r2, lr
        submi   r0, r0, r6
        addpl   r0, r0, r6
        cmp     r0, #1024               @ then clip weight to +/-1024
        movgt   r0, #1024
        cmp     r0, r10
        movlt   r0, r10

S369:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     term_minus_1_loop

        str     r3, [r5, #16]           @ else store right sample and exit
        b       common_exit

/*
 ******************************************************************************
 * Loop to handle term = -2 condition
 * (note that the channels are processed in the reverse order here)
 *
 * r0 = dpp->weight_B           r8 =
 * r1 = bptr                    r9 =
 * r2 = intermediate result     r10 = -1024 (for clipping)
 * r3 = previous left sample    r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = updated right sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

term_minus_2:
        ldr     r3, [r1, #-8]

term_minus_2_loop:
        ldr     ip, [r1, #4]            @ for right channel the decorrelation value
        mla     r2, r3, r0, r11         @  is the previous left sample (in r3)
        add     lr, ip, r2, asr #10
        str     lr, [r1, #4]
        cmp     r3, #0
        cmpne   ip, #0
        beq     S380
        teq     ip, r3                  @ update weight based on signs
        submi   r0, r0, r6
        addpl   r0, r0, r6
        cmp     r0, #1024               @ then clip weight to +/-1024
        movgt   r0, #1024
        cmp     r0, r10
        movlt   r0, r10

S380:   ldr     r2, [r1, #0]            @ for left channel the decorrelation value
        mla     r3, lr, r4, r11         @  is the just updated left sample (in lr)
        add     r3, r2, r3, asr #10
        str     r3, [r1], #8
        cmp     lr, #0
        cmpne   r2, #0
        beq     S388
        teq     r2, lr
        submi   r4, r4, r6
        addpl   r4, r4, r6
        cmp     r4, #1024
        movgt   r4, #1024
        cmp     r4, r10
        movlt   r4, r10

S388:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     term_minus_2_loop

        str     r3, [r5, #48]           @ else store left channel and exit
        b       common_exit

/*
 ******************************************************************************
 * Loop to handle term = -3 condition
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current left sample     r10 = -1024 (for clipping)
 * r3 = previous right sample   r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = intermediate result
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr =
 * r7 = eptr                    pc =
 *******************************************************************************
 */

term_minus_3:
        ldr     r3, [r1, #-4]           @ load previous samples
        ldr     r8, [r1, #-8]

term_minus_3_loop:
        ldr     ip, [r1]
        mla     r2, r3, r4, r11
        add     r2, ip, r2, asr #10
        str     r2, [r1], #4
        cmp     r3, #0
        cmpne   ip, #0
        beq     S399
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6
        cmp     r4, #1024               @ then clip weight to +/-1024
        movgt   r4, #1024
        cmp     r4, r10
        movlt   r4, r10

S399:   movs    ip, r8                  @ ip = previous left we use now
        mov     r8, r2                  @ r8 = current left we use next time
        ldr     r2, [r1], #4
        mla     r3, ip, r0, r11
        add     r3, r2, r3, asr #10
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     S407
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6
        cmp     r0, #1024
        movgt   r0, #1024
        cmp     r0, r10
        movlt   r0, r10

S407:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     term_minus_3_loop

        str     r3, [r5, #16]           @ else store previous samples & exit
        str     r8, [r5, #48]

/*
 * Before finally exiting we must store weights back for next time
 */

common_exit:
        str     r4, [r5, #8]
        str     r0, [r5, #12]
        ldmfd   sp!, {r4 - r8, r10, r11, pc}

/*
 * on entry:
 *
 * r0 = struct decorr_pass *dpp
 * r1 = int32_t *buffer
 * r2 = int32_t sample_count
 */

long_versions:
        mov     r5, r0                  @ r5 = dpp
        mov     r11, #512               @ r11 = 512 for rounding
        ldr     r6, [r0, #4]            @ r6 = dpp->delta
        ldr     r4, [r0, #8]            @ r4 = dpp->weight_A
        ldr     r0, [r0, #12]           @ r0 = dpp->weight_B
        mov     r0, r0, asl #18         @ for 64-bit math we use weights << 18
        mov     r4, r4, asl #18
        mov     r6, r6, asl #18
        cmp     r2, #0                  @ exit if no samples to process
        beq     long_common_exit

        add     r7, r1, r2, asl #3      @ r7 = buffer ending position
        ldr     r2, [r5, #0]            @ r2 = dpp->term
        cmp     r2, #0
        blt     long_minus_term

        ldr     lr, [r1, #-16]          @ load 2 sample history from buffer
        ldr     r10, [r1, #-12]         @  for terms 2, 17, and 18
        ldr     r8, [r1, #-8]
        ldr     r3, [r1, #-4]

        cmp     r2, #18
        beq     long_term_18_loop
        mov     lr, lr, asl #4
        mov     r10, r10, asl #4
        cmp     r2, #2
        beq     long_term_2_loop
        cmp     r2, #17
        beq     long_term_17_loop
        b       long_term_default_loop

long_minus_term:
        mov     r10, #(1024 << 18)      @ r10 = -1024 << 18 for weight clipping
        rsb     r10, r10, #0            @  (only used for negative terms)
        cmn     r2, #1
        beq     long_term_minus_1
        cmn     r2, #2
        beq     long_term_minus_2
        cmn     r2, #3
        beq     long_term_minus_3
        b       long_common_exit

/*
 ******************************************************************************
 * Loop to handle term = 17 condition
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 = second previous right sample << 4
 * r3 = previous right sample   r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = current decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous left sample << 4
 * r7 = eptr                    pc =
 *******************************************************************************
 */

long_term_17_loop:
        rsbs    ip, lr, r8, asl #5      @ decorr value = (2 * prev) - 2nd prev
        mov     lr, r8, asl #4          @ previous becomes 2nd previous
        ldr     r2, [r1], #4            @ get sample & update pointer
        mov     r11, #0x80000000
        mov     r8, r2
        smlalne r11, r8, r4, ip
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     L325
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L325:   rsbs    ip, r10, r3, asl #5     @ do same thing for right channel
        mov     r10, r3, asl #4
        ldr     r2, [r1], #4
        mov     r11, #0x80000000
        mov     r3, r2
        smlalne r11, r3, r0, ip
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     L329
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6

L329:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     long_term_17_loop
        mov     lr, lr, asr #4
        mov     r10, r10, asr #4
        b       long_store_1718         @ common exit for terms 17 & 18

/*
 ******************************************************************************
 * Loop to handle term = 18 condition
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 = second previous right sample
 * r3 = previous right sample   r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous left sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

long_term_18_loop:
        rsb     ip, lr, r8              @ decorr value =
        mov     lr, r8                  @  ((3 * prev) - 2nd prev) >> 1
        add     ip, lr, ip, asr #1
        movs    ip, ip, asl #4
        ldr     r2, [r1], #4            @ get sample & update pointer
        mov     r11, #0x80000000
        mov     r8, r2
        smlalne r11, r8, r4, ip
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     L337
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L337:   rsb     ip, r10, r3             @ do same thing for right channel
        mov     r10, r3
        add     ip, r10, ip, asr #1
        movs    ip, ip, asl #4
        ldr     r2, [r1], #4
        mov     r11, #0x80000000
        mov     r3, r2
        smlalne r11, r3, r0, ip
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     L341
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6

L341:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     long_term_18_loop

/* common exit for terms 17 & 18 */

long_store_1718:
        str     r3, [r5, #48]           @ store sample history into struct
        str     r8, [r5, #16]
        str     r10, [r5, #52]
        str     lr, [r5, #20]
        b       long_common_exit        @ and return

/*
 ******************************************************************************
 * Loop to handle term = 2 condition
 * (note that this case can be handled by the default term handler (1-8), but
 * this special case is faster because it doesn't have to read memory twice)
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 = second previous right sample << 4
 * r3 = previous right sample   r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous left sample << 4
 * r7 = eptr                    pc =
 *******************************************************************************
 */

long_term_2_loop:
        movs    ip, lr                  @ get decorrelation value & test
        ldr     r2, [r1], #4            @ get sample & update pointer
        mov     lr, r8, asl #4          @ previous becomes 2nd previous
        mov     r11, #0x80000000
        mov     r8, r2
        smlalne r11, r8, r4, ip
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     L225
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L225:   movs    ip, r10                 @ do same thing for right channel
        ldr     r2, [r1], #4
        mov     r10, r3, asl #4
        mov     r11, #0x80000000
        mov     r3, r2
        smlalne r11, r3, r0, ip
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     L229
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6

L229:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     long_term_2_loop

        b       long_default_term_exit  @ this exit updates all dpp->samples

/*
 ******************************************************************************
 * Loop to handle default term condition
 *
 * r0 = dpp->weight_B           r8 = result accumulator
 * r1 = bptr                    r9 =
 * r2 = dpp->term               r10 =
 * r3 = decorrelation value     r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr =
 * r7 = eptr                    pc =
 *******************************************************************************
 */

long_term_default_loop:
        ldr     r3, [r1, -r2, asl #3]   @ get decorrelation value based on term
        ldr     ip, [r1], #4            @ get original sample and bump ptr
        movs    r3, r3, asl #4
        mov     r11, #0x80000000
        mov     r8, ip
        smlalne r11, r8, r4, r3
        strne   r8, [r1, #-4]           @ if possibly changed, store updated sample
        cmpne   ip, #0
        beq     L350
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L350:   ldr     r3, [r1, -r2, asl #3]   @ do the same thing for right channel
        ldr     ip, [r1], #4
        movs    r3, r3, asl #4
        mov     r11, #0x80000000
        mov     r8, ip
        smlalne r11, r8, r0, r3
        strne   r8, [r1, #-4]
        cmpne   ip, #0
        beq     L354
        teq     ip, r3
        submi   r0, r0, r6
        addpl   r0, r0, r6

L354:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     long_term_default_loop

/*
 * This exit is used by terms 1-8 to store the previous "term" samples (up to 8)
 * into the decorr pass structure history
 */

long_default_term_exit:
        ldr     r2, [r5, #0]            @ r2 = dpp->term

L358:   sub     r2, r2, #1
        sub     r1, r1, #8
        ldr     r3, [r1, #4]            @ get right sample and store in dpp->samples_B [r2]
        add     r6, r5, #48
        str     r3, [r6, r2, asl #2]
        ldr     r3, [r1, #0]            @ get left sample and store in dpp->samples_A [r2]
        add     r6, r5, #16
        str     r3, [r6, r2, asl #2]
        cmp     r2, #0
        bne     L358
        b       long_common_exit

/*
 ******************************************************************************
 * Loop to handle term = -1 condition
 *
 * r0 = dpp->weight_B           r8 =
 * r1 = bptr                    r9 =
 * r2 = intermediate result     r10 = -1024 (for clipping)
 * r3 = previous right sample   r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = updated left sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

long_term_minus_1:
        ldr     r3, [r1, #-4]

long_term_minus_1_loop:
        ldr     ip, [r1], #8            @ for left channel the decorrelation value
        movs    r3, r3, asl #4          @  is the previous right sample (in r3)
        mov     r11, #0x80000000
        mov     lr, ip
        smlalne r11, lr, r4, r3
        strne   lr, [r1, #-8]
        cmpne   ip, #0
        beq     L361
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6
        cmp     r4, #(1024 << 18)
        movgt   r4, #(1024 << 18)
        cmp     r4, r10
        movlt   r4, r10

L361:   ldr     r2, [r1, #-4]           @ for right channel the decorrelation value
        movs    lr, lr, asl #4
        mov     r11, #0x80000000
        mov     r3, r2
        smlalne r11, r3, r0, lr
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     L369
        teq     r2, lr
        submi   r0, r0, r6
        addpl   r0, r0, r6
        cmp     r0, #(1024 << 18)               @ then clip weight to +/-1024
        movgt   r0, #(1024 << 18)
        cmp     r0, r10
        movlt   r0, r10

L369:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     long_term_minus_1_loop

        str     r3, [r5, #16]           @ else store right sample and exit
        b       long_common_exit

/*
 ******************************************************************************
 * Loop to handle term = -2 condition
 * (note that the channels are processed in the reverse order here)
 *
 * r0 = dpp->weight_B           r8 =
 * r1 = bptr                    r9 =
 * r2 = intermediate result     r10 = -1024 (for clipping)
 * r3 = previous left sample    r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = updated right sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

long_term_minus_2:
        ldr     r3, [r1, #-8]

long_term_minus_2_loop:
        ldr     ip, [r1, #4]            @ for right channel the decorrelation value
        movs    r3, r3, asl #4          @  is the previous left sample (in r3)
        mov     r11, #0x80000000
        mov     lr, ip
        smlalne r11, lr, r0, r3
        strne   lr, [r1, #4]
        cmpne   ip, #0
        beq     L380
        teq     ip, r3                  @ update weight based on signs
        submi   r0, r0, r6
        addpl   r0, r0, r6
        cmp     r0, #(1024 << 18)               @ then clip weight to +/-1024
        movgt   r0, #(1024 << 18)
        cmp     r0, r10
        movlt   r0, r10

L380:   ldr     r2, [r1], #8            @ for left channel the decorrelation value
        movs    lr, lr, asl #4
        mov     r11, #0x80000000
        mov     r3, r2
        smlalne r11, r3, r4, lr
        strne   r3, [r1, #-8]
        cmpne   r2, #0
        beq     L388
        teq     r2, lr
        submi   r4, r4, r6
        addpl   r4, r4, r6
        cmp     r4, #(1024 << 18)
        movgt   r4, #(1024 << 18)
        cmp     r4, r10
        movlt   r4, r10

L388:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     long_term_minus_2_loop

        str     r3, [r5, #48]           @ else store left channel and exit
        b       long_common_exit

/*
 ******************************************************************************
 * Loop to handle term = -3 condition
 *
 * r0 = dpp->weight_B           r8 = previous left sample
 * r1 = bptr                    r9 =
 * r2 = current left sample     r10 = -1024 (for clipping)
 * r3 = previous right sample   r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = intermediate result
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr =
 * r7 = eptr                    pc =
 *******************************************************************************
 */

long_term_minus_3:
        ldr     r3, [r1, #-4]           @ load previous samples
        ldr     r8, [r1, #-8]

long_term_minus_3_loop:
        ldr     ip, [r1], #4
        movs    r3, r3, asl #4
        mov     r11, #0x80000000
        mov     r2, ip
        smlalne r11, r2, r4, r3
        strne   r2, [r1, #-4]
        cmpne   ip, #0
        beq     L399
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6
        cmp     r4, #(1024 << 18)       @ then clip weight to +/-1024
        movgt   r4, #(1024 << 18)
        cmp     r4, r10
        movlt   r4, r10

L399:   movs    ip, r8, asl #4          @ ip = previous left we use now
        mov     r8, r2                  @ r8 = current left we use next time
        ldr     r2, [r1], #4
        mov     r11, #0x80000000
        mov     r3, r2
        smlalne r11, r3, r0, ip
        strne   r3, [r1, #-4]
        cmpne   r2, #0
        beq     L407
        teq     ip, r2
        submi   r0, r0, r6
        addpl   r0, r0, r6
        cmp     r0, #(1024 << 18)
        movgt   r0, #(1024 << 18)
        cmp     r0, r10
        movlt   r0, r10

L407:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     long_term_minus_3_loop

        str     r3, [r5, #16]           @ else store previous samples & exit
        str     r8, [r5, #48]

/*
 * Before finally exiting we must store weights back for next time
 */

long_common_exit:
        mov     r0, r0, asr #18         @ restore weights to real magnitude
        mov     r4, r4, asr #18
        str     r4, [r5, #8]
        str     r0, [r5, #12]
        ldmfd   sp!, {r4 - r8, r10, r11, pc}


/* This is a mono version of the function above. It does not handle negative terms.
 *
 * void decorr_mono_pass_cont (struct decorr_pass *dpp,
 *                             int32_t *buffer,
 *                             int32_t sample_counti,
 *                             int32_t long_math);
 * on entry:
 *
 * r0 = struct decorr_pass *dpp
 * r1 = int32_t *buffer
 * r2 = int32_t sample_count
 * r3 = int32_t long_math
 */

unpack_decorr_mono_pass_cont_armv7:

        stmfd   sp!, {r4 - r8, r11, lr}
        cmp     r3, #0                  @ check for long versions required
        bne     mono_long_versions      @ branch if yes

        mov     r5, r0                  @ r5 = dpp
        mov     r11, #512               @ r11 = 512 for rounding
        ldr     r6, [r0, #4]            @ r6 = dpp->delta
        ldr     r4, [r0, #8]            @ r4 = dpp->weight_A
        cmp     r2, #0                  @ exit if no samples to process
        beq     mono_common_exit

        add     r7, r1, r2, asl #2      @ r7 = buffer ending position
        ldr     r2, [r5, #0]            @ r2 = dpp->term

        ldr     lr, [r1, #-8]           @ load 2 sample history from buffer
        ldr     r8, [r1, #-4]
        cmp     r2, #17
        beq     mono_term_17_loop
        cmp     r2, #18
        beq     mono_term_18_loop
        cmp     r2, #2
        beq     mono_term_2_loop
        b       mono_term_default_loop  @ else handle default (1-8, except 2)

/*
 ******************************************************************************
 * Loop to handle term = 17 condition
 *
 * r0 =                         r8 = previous sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 =
 * r3 =                         r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = current decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_term_17_loop:
        rsbs    ip, lr, r8, asl #1      @ decorr value = (2 * prev) - 2nd prev
        mov     lr, r8                  @ previous becomes 2nd previous
        ldr     r2, [r1], #4            @ get sample & update pointer
        mla     r8, ip, r4, r11         @ mult decorr value by weight, round,
        add     r8, r2, r8, asr #10     @  shift, and add to new sample
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     S129
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S129:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_term_17_loop
        b       mono_store_1718         @ common exit for terms 17 & 18

/*
 ******************************************************************************
 * Loop to handle term = 18 condition
 *
 * r0 =                         r8 = previous sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 =
 * r3 =                         r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_term_18_loop:
        sub     ip, r8, lr              @ decorr value =
        mov     lr, r8                  @  ((3 * prev) - 2nd prev) >> 1
        adds    ip, r8, ip, asr #1
        ldr     r2, [r1], #4            @ get sample & update pointer
        mla     r8, ip, r4, r11         @ mult decorr value by weight, round,
        add     r8, r2, r8, asr #10     @  shift, and add to new sample
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     S141
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S141:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_term_18_loop

/* common exit for terms 17 & 18 */

mono_store_1718:
        str     r8, [r5, #16]           @ store sample history into struct
        str     lr, [r5, #20]
        b       mono_common_exit        @ and return

/*
 ******************************************************************************
 * Loop to handle term = 2 condition
 * (note that this case can be handled by the default term handler (1-8), but
 * this special case is faster because it doesn't have to read memory twice)
 *
 * r0 =                         r8 = previous sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 =
 * r3 =                         r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_term_2_loop:
        movs    ip, lr                  @ get decorrelation value & test
        mov     lr, r8                  @ previous becomes 2nd previous
        ldr     r2, [r1], #4            @ get sample & update pointer
        mla     r8, ip, r4, r11         @ mult decorr value by weight, round,
        add     r8, r2, r8, asr #10     @  shift, and add to new sample
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     S029
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S029:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_term_2_loop
        b       mono_default_term_exit  @ this exit updates all dpp->samples

/*
 ******************************************************************************
 * Loop to handle default term condition
 *
 * r0 =                         r8 = result accumulator
 * r1 = bptr                    r9 =
 * r2 = dpp->term               r10 =
 * r3 = decorrelation value     r11 = 512 (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr =
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_term_default_loop:
        ldr     ip, [r1]                @ get original sample
        ldr     r3, [r1, -r2, asl #2]   @ get decorrelation value based on term
        mla     r8, r3, r4, r11         @ mult decorr value by weight, round,
        add     r8, ip, r8, asr #10     @  shift and add to new sample
        str     r8, [r1], #4            @ store update sample
        cmp     r3, #0
        cmpne   ip, #0
        beq     S154
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

S154:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_term_default_loop

/*
 * This exit is used by terms 1-8 to store the previous "term" samples (up to 8)
 * into the decorr pass structure history
 */

mono_default_term_exit:
        ldr     r2, [r5, #0]            @ r2 = dpp->term

S158:   sub     r2, r2, #1
        sub     r1, r1, #4
        ldr     r3, [r1, #0]            @ get sample and store in dpp->samples_A [r2]
        add     r6, r5, #16
        str     r3, [r6, r2, asl #2]
        cmp     r2, #0
        bne     S158
        b       mono_common_exit

/*
 * Before finally exiting we must store weight back for next time
 */

mono_common_exit:
        str     r4, [r5, #8]
        ldmfd   sp!, {r4 - r8, r11, pc}

/*
 * on entry:
 *
 * r0 = struct decorr_pass *dpp
 * r1 = int32_t *buffer
 * r2 = int32_t sample_count
 */

mono_long_versions:
        mov     r5, r0                  @ r5 = dpp
        mov     r11, #512               @ r11 = 512 for rounding
        ldr     r6, [r0, #4]            @ r6 = dpp->delta
        ldr     r4, [r0, #8]            @ r4 = dpp->weight_A
        mov     r4, r4, asl #18
        mov     r6, r6, asl #18
        cmp     r2, #0                  @ exit if no samples to process
        beq     mono_long_common_exit

        add     r7, r1, r2, asl #2      @ r7 = buffer ending position
        ldr     r2, [r5, #0]            @ r2 = dpp->term

        ldr     lr, [r1, #-8]           @ load 2 sample history from buffer
        ldr     r8, [r1, #-4]           @  for terms 2, 17, and 18

        cmp     r2, #18
        beq     mono_long_term_18_loop
        mov     lr, lr, asl #4
        cmp     r2, #2
        beq     mono_long_term_2_loop
        cmp     r2, #17
        beq     mono_long_term_17_loop
        b       mono_long_term_default_loop

/*
 ******************************************************************************
 * Loop to handle term = 17 condition
 *
 * r0 =                         r8 = previous sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 =
 * r3 =                         r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = current decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous sample << 4
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_long_term_17_loop:
        rsbs    ip, lr, r8, asl #5      @ decorr value = (2 * prev) - 2nd prev
        mov     lr, r8, asl #4          @ previous becomes 2nd previous
        ldr     r2, [r1], #4            @ get sample & update pointer
        mov     r11, #0x80000000
        mov     r8, r2
        smlalne r11, r8, r4, ip
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     L129
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L129:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_long_term_17_loop
        mov     lr, lr, asr #4
        b       mono_long_store_1718    @ common exit for terms 17 & 18

/*
 ******************************************************************************
 * Loop to handle term = 18 condition
 *
 * r0 =                         r8 = previous sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 =
 * r3 =                         r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous sample
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_long_term_18_loop:
        rsb     ip, lr, r8              @ decorr value =
        mov     lr, r8                  @  ((3 * prev) - 2nd prev) >> 1
        add     ip, lr, ip, asr #1
        movs    ip, ip, asl #4
        ldr     r2, [r1], #4            @ get sample & update pointer
        mov     r11, #0x80000000
        mov     r8, r2
        smlalne r11, r8, r4, ip
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     L141
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L141:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_long_term_18_loop

/* common exit for terms 17 & 18 */

mono_long_store_1718:
        str     r8, [r5, #16]           @ store sample history into struct
        str     lr, [r5, #20]
        b       mono_long_common_exit   @ and return

/*
 ******************************************************************************
 * Loop to handle term = 2 condition
 * (note that this case can be handled by the default term handler (1-8), but
 * this special case is faster because it doesn't have to read memory twice)
 *
 * r0 =                         r8 = previous sample
 * r1 = bptr                    r9 =
 * r2 = current sample          r10 =
 * r3 =                         r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = decorrelation value
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr = second previous sample << 4
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_long_term_2_loop:
        movs    ip, lr                  @ get decorrelation value & test
        ldr     r2, [r1], #4            @ get sample & update pointer
        mov     lr, r8, asl #4          @ previous becomes 2nd previous
        mov     r11, #0x80000000
        mov     r8, r2
        smlalne r11, r8, r4, ip
        strne   r8, [r1, #-4]           @ if change possible, store sample back
        cmpne   r2, #0
        beq     L029
        teq     ip, r2                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L029:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_long_term_2_loop

        b       mono_long_default_term_exit  @ this exit updates all dpp->samples

/*
 ******************************************************************************
 * Loop to handle default term condition
 *
 * r0 =                         r8 = result accumulator
 * r1 = bptr                    r9 =
 * r2 = dpp->term               r10 =
 * r3 = decorrelation value     r11 = lo accumulator (for rounding)
 * r4 = dpp->weight_A           ip = current sample
 * r5 = dpp                     sp =
 * r6 = dpp->delta              lr =
 * r7 = eptr                    pc =
 *******************************************************************************
 */

mono_long_term_default_loop:
        ldr     r3, [r1, -r2, asl #2]   @ get decorrelation value based on term
        ldr     ip, [r1], #4            @ get original sample and bump ptr
        movs    r3, r3, asl #4
        mov     r11, #0x80000000
        mov     r8, ip
        smlalne r11, r8, r4, r3
        strne   r8, [r1, #-4]           @ if possibly changed, store updated sample
        cmpne   ip, #0
        beq     L154
        teq     ip, r3                  @ update weight based on signs
        submi   r4, r4, r6
        addpl   r4, r4, r6

L154:   cmp     r7, r1                  @ loop back if more samples to do
        bhi     mono_long_term_default_loop

/*
 * This exit is used by terms 1-8 to store the previous "term" samples (up to 8)
 * into the decorr pass structure history
 */

mono_long_default_term_exit:
        ldr     r2, [r5, #0]            @ r2 = dpp->term

L158:   sub     r2, r2, #1
        sub     r1, r1, #4
        ldr     r3, [r1, #0]            @ get sample and store in dpp->samples_A [r2]
        add     r6, r5, #16
        str     r3, [r6, r2, asl #2]
        cmp     r2, #0
        bne     L158
        b       mono_long_common_exit

/*
 * Before finally exiting we must store weights back for next time
 */

mono_long_common_exit:
        mov     r4, r4, asr #18         @ restore weight to real magnitude
        str     r4, [r5, #8]
        ldmfd   sp!, {r4 - r8, r11, pc}

