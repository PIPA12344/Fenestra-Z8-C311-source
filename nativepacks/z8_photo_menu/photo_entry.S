/* SPDX-License-Identifier: GPL-3.0-only */
.section .photo_prefix,"ax",%progbits
.global photo_open_hook
.type photo_open_hook,%function
photo_open_hook:
    stp x9, x10, [sp, #-16]!
    mrs x10, nzcv
    movz x9, #0xdfa0
    movk x9, #0x401c, lsl #16
    cmp x30, x9
    b.ne .Lstock_open
    orr w9, w1, w2
    cbnz w9, .Lstock_open
    msr nzcv, x10
    ldp x9, x10, [sp], #16
    b .Lscoped_open
.Lstock_open:
    msr nzcv, x10
    ldp x9, x10, [sp], #16
    b photo_stock_open
.size photo_open_hook, . - photo_open_hook

.org 0x80
.global photo_stock_open
.type photo_stock_open,%function
photo_stock_open:
    .inst 0xf81d0ff6 /* stock open entry: str x22, [sp, #-0x30]! */
    .inst 0x17ddf51e /* b 0x402dd4fc */
.size photo_stock_open, . - photo_stock_open

.org 0x100
.global photo_text_shim
.type photo_text_shim,%function
photo_text_shim:
    mov w9, #0x1f40
    sub w9, w1, w9
    cmp w9, #11
    b.lo photo_text_lookup
    b photo_stock_text
.size photo_text_shim, . - photo_text_shim

/* The stock call uses its ordinary ABI. Preserve every volatile GPR and
 * NZCV around our helpers, including the complete stock return register. */
.macro save_open_registers
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    mrs x9, nzcv
    stp x18, x9, [sp, #144]
.endm

.macro restore_open_registers
    ldp x18, x9, [sp, #144]
    msr nzcv, x9
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
.endm

.Lscoped_open:
    sub sp, sp, #176
    str x30, [sp, #160]
    save_open_registers
    bl photo_open_arm
    str x0, [sp, #168]
    restore_open_registers
    bl photo_stock_open
    save_open_registers
    ldr x0, [sp, #168]
    cbz x0, .Lopen_return
    bl photo_open_complete
.Lopen_return:
    restore_open_registers
    ldr x30, [sp, #160]
    add sp, sp, #176
    ret

/* Burst-only compositions have no v2 rear gate. Hook the sender's adjacent
 * load sites so the legacy rear's SYNC call hooks can still compose. */
.macro burst_power_callback cancel, continuation
    sub sp, sp, #176
    str x30, [sp, #160]
    save_open_registers
    mov w0, #\cancel
    bl photo_burst_power
    restore_open_registers
    ldr x30, [sp, #160]
    add sp, sp, #176
    ldr w1, [x19]
    b \continuation
.endm

.org 0x240
.global photo_burst_power_off
.type photo_burst_power_off,%function
photo_burst_power_off:
    burst_power_callback 0, photo_power_off_continue
.size photo_burst_power_off, . - photo_burst_power_off

.org 0x2c0
.global photo_burst_power_cancel
.type photo_burst_power_cancel,%function
photo_burst_power_cancel:
    burst_power_callback 1, photo_power_cancel_continue
.size photo_burst_power_cancel, . - photo_burst_power_cancel

/* Providers may call this fixed v4 veneer after matching its ABI marker. */
.org 0x340
.global photo_flight_entry
.type photo_flight_entry,%function
photo_flight_entry:
    b photo_flight_log
.size photo_flight_entry, . - photo_flight_entry
.word 0x46523431
.global photo_heartbeat_abi
photo_heartbeat_abi:
.word 0x48425431
