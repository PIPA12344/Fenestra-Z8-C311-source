ENTRY(fx_native_pack_entry)
PHDRS {
    code PT_LOAD FLAGS(5);
    constants PT_LOAD FLAGS(4);
    state PT_LOAD FLAGS(6);
}
SECTIONS {
    .text 0x40bf0000 : { *(.text*) } :code
    .rodata 0x40bf7000 : { *(.rodata*) } :constants
    .data 0x40bf9000 : { *(.data*) *(.bss*) } :state
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
ASSERT(SIZEOF(.text) <= 0x7000, "preview code capacity")
ASSERT(SIZEOF(.rodata) <= 0x2000, "preview constants capacity")
ASSERT(SIZEOF(.data) <= 0x6000, "preview state capacity")
