/* SPDX-License-Identifier: GPL-3.0-only */
ENTRY(fx_native_pack_entry)
PHDRS {
  code PT_LOAD FLAGS(5);
  resources PT_LOAD FLAGS(4);
  state PT_LOAD FLAGS(6);
  stack PT_LOAD FLAGS(6);
}
SECTIONS {
  . = 0x40b60000;
  .text : ALIGN(16) {
    KEEP(*(.photo_prefix))
    *(.text*)
  } :code
  ASSERT(. <= 0x40b6c000, "photo menu RX exceeded")
  . = 0x40b6c000;
  .photo_layout : { KEEP(*(.photo_layout)) } :resources
  .rodata : ALIGN(8) { *(.rodata*) } :resources
  ASSERT(. <= 0x40b70000, "photo menu RO exceeded")
  . = 0x40b70000;
  .data : ALIGN(16) { *(.data*) } :state
  .bss (NOLOAD) : ALIGN(16) { *(.bss.write_buffer) *(.bss*) *(COMMON) } :state
  ASSERT(. <= 0x40b73000, "photo menu state overlaps the flight recorder")
  . = 0x40b73000;
  .flight (NOLOAD) : ALIGN(64) {
    KEEP(*(.flight.records))
    KEEP(*(.flight.head))
    KEEP(*(.flight.heartbeats))
    KEEP(*(.flight.watchdog))
  } :state
  ASSERT(photo_flight_ring == 0x40b73000 && photo_flight_head == 0x40b73800,
         "photo menu flight recorder ABI moved")
  ASSERT(photo_heartbeat_slots == 0x40b73808 && photo_watchdog_status == 0x40b73908 &&
         . == 0x40b73928, "photo menu heartbeat or watchdog ABI moved")
  .browser (NOLOAD) : ALIGN(16) { KEEP(*(.browser)) } :state
  ASSERT(. <= 0x40b78000, "photo menu RW exceeded")
  . = 0x40b78000;
  .stack (NOLOAD) : ALIGN(16) { KEEP(*(.stack)) } :stack
  ASSERT(. == 0x40b80000, "photo menu worker stack extent")
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) }
  PROVIDE(photo_stock_text = 0x4032e130);
  PROVIDE(photo_power_off_continue = 0x40d6733c);
  PROVIDE(photo_power_cancel_continue = 0x40d67330);
}
