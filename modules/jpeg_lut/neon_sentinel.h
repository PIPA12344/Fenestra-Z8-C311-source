/* Instantiate with FX_NEON_SENTINEL set to the pack's isolated symbol.
 * TASK_DELAY may use caller-saved vectors before the scheduler saves them.
 * Test the PCS-preserved D registers across that call. Then test all Q
 * registers through a 3 ms IRQ window with no calls in the window. */
#define SIMD_REG_GROUP(op, lo, hi) op " {v" #lo ".16b-v" #hi ".16b}, [x9], #64\n"
#define SIMD_ALL_REGS(op) \
    SIMD_REG_GROUP(op, 0, 3) SIMD_REG_GROUP(op, 4, 7) \
    SIMD_REG_GROUP(op, 8, 11) SIMD_REG_GROUP(op, 12, 15) \
    SIMD_REG_GROUP(op, 16, 19) SIMD_REG_GROUP(op, 20, 23) \
    SIMD_REG_GROUP(op, 24, 27) SIMD_REG_GROUP(op, 28, 31)
#define SIMD_D_PAIRS(op, base, offset) \
    op " d8, d9, [" base ", #" #offset "]\n" \
    op " d10, d11, [" base ", #" #offset "+16]\n" \
    op " d12, d13, [" base ", #" #offset "+32]\n" \
    op " d14, d15, [" base ", #" #offset "+48]\n"
#define SIMD_VERIFY(lanes) \
    "add x9, sp, #96\n" \
    "adr x10, 8f\n" \
    "mov w11, #" #lanes "\n" \
    "7: ldr x12, [x9], #8\n" \
    "ldr x13, [x10], #8\n" \
    "cmp x12, x13\n" \
    "b.ne 2f\n" \
    "subs w11, w11, #1\n" \
    "b.ne 7b\n"

__attribute__((naked)) unsigned FX_NEON_SENTINEL(int32_t (*delay)(uint32_t) __attribute__((unused))) {
    __asm__ volatile(
        "mrs x9, cpacr_el1\n"
        "tbz x9, #20, 5f\n"
        "mrs x9, daif\n"
        "tbnz x9, #7, 5f\n"
        "sub sp, sp, #608\n"
        "stp x29, x30, [sp]\n"
        "mov x29, sp\n"
        SIMD_D_PAIRS("stp", "sp", 16)
        "mrs x9, fpcr\n"
        "mrs x10, fpsr\n"
        "stp x9, x10, [sp, #80]\n"
        "adr x9, 8f\n"
        SIMD_D_PAIRS("ldp", "x9", 0)
        "mov x9, x0\n"
        "mov w0, #1\n"
        "blr x9\n"
        "mrs x9, cpacr_el1\n"
        "tbz x9, #20, 4f\n"
        "cbnz w0, 2f\n"
        SIMD_D_PAIRS("stp", "sp", 96)
        SIMD_VERIFY(8)

        "adr x9, 8f\n"
        SIMD_ALL_REGS("ld1")
        "mrs x11, cntpct_el0\n"
        "movz w12, #0x1940\n"
        "movk w12, #1, lsl #16\n" /* 72,000 ticks at 24 MHz */
        "1: mrs x13, cntpct_el0\n"
        "sub x13, x13, x11\n"
        "cmp x13, x12\n"
        "b.lo 1b\n"
        "mrs x9, cpacr_el1\n"
        "tbz x9, #20, 4f\n"
        "add x9, sp, #96\n"
        SIMD_ALL_REGS("st1")
        SIMD_VERIFY(64)
        "mov w0, #1\n"
        "b 3f\n"
        "2: mov w0, #2\n"
        "3:\n"
        SIMD_D_PAIRS("ldp", "sp", 16)
        "ldp x9, x10, [sp, #80]\n"
        "msr fpcr, x9\n"
        "msr fpsr, x10\n"
        "b 6f\n"

        /* FP was initially available in this stock EL1 context. If CPACR
         * changes during the probe, restore the caller's PCS state inside
         * a masked EL1-only FP window, then reinstate the revoked CPACR. */
        "4: mov w0, #2\n"
        "mrs x11, daif\n"
        "msr daifset, #15\n"
        "orr x10, x9, #0x100000\n"
        "msr cpacr_el1, x10\n"
        "isb\n"
        SIMD_D_PAIRS("ldp", "sp", 16)
        "ldp x12, x13, [sp, #80]\n"
        "msr fpcr, x12\n"
        "msr fpsr, x13\n"
        "msr cpacr_el1, x9\n"
        "isb\n"
        "msr daif, x11\n"
        "6: ldp x29, x30, [sp]\n"
        "add sp, sp, #608\n"
        "ret\n"
        "5: mov w0, #0\n"
        "ret\n"
        ".pushsection .rodata.jpeg_neon_patterns, \"a\", @progbits\n"
        ".balign 16\n"
        "8:\n"
        ".set jpeg_lane, 0\n"
        ".rept 64\n"
        ".quad 0x5eb19c73d284a60f ^ (jpeg_lane * 0x9e3779b97f4a7c15)\n"
        ".set jpeg_lane, (jpeg_lane + 1)\n"
        ".endr\n"
        ".popsection\n"
    );
}
