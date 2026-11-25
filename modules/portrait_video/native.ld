/* SPDX-License-Identifier: GPL-3.0-only */
ENTRY(fx_native_pack_entry)
PHDRS {
    code PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    state PT_LOAD FLAGS(6);
#ifdef PORTRAIT_FXLOG
    fxlog PT_LOAD FLAGS(6);
#endif
}
SECTIONS {
    .text 0x40be0000 : { *(.text*) } :code
    .rodata 0x40be1000 : { *(.rodata*) } :rodata
    .data 0x40be2000 : { *(.data*) *(.bss*) } :state
#ifdef PORTRAIT_FXLOG
    .fxlog 0x40be3000 : { *(.fxlog.rw) } :fxlog
    ASSERT(SIZEOF(.fxlog) > 0 && SIZEOF(.fxlog) <= 4096, "portrait fxlog exceeds its RW page")
#endif
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
