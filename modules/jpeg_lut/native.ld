ENTRY(fx_native_pack_entry)
PHDRS {
    code PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    state PT_LOAD FLAGS(6);
}
SECTIONS {
    .text 0x40be6000 : { *(.text*) } :code
    .rodata 0x40bee000 : { *(.rodata*) } :rodata
    .data 0x40bef000 : { *(.data.fx_z8_jpeg_phase_telemetry) *(.data*) *(.bss*) *(.burst_data) } :state
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
ASSERT(SIZEOF(.text) <= 0x8000, "JPEG LUT code capacity")
ASSERT(SIZEOF(.rodata) <= 0x1000, "JPEG LUT constants capacity")
ASSERT(SIZEOF(.data) <= 0x1000, "JPEG LUT state capacity")
