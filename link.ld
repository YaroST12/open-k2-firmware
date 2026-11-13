/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Yaraslau Furman
 */
/* Linker script for WITRN K2 (Artery AT32F403AVGT7)
 * The bootloader resides at 0x08000000 - 0x08003800 (14 KB)
 * Application begins at 0x08003800
 *
 * Fixed K2 hardware offsets:
 * 0x08003800: Vector table (SP & Reset Vector)
 * 0x08004000: Firmware Version Word (Offset 2048)
 * 0x08004010: Model Identification Signature "K200" (Offset 2064, checked by bootloader)
 * 0x08004400: Bootloader DFU Commit Marker (Offset 3072)
 * 0x08004404: Application Code (.text, .rodata); its first 48 bytes are the "K2FW" version record (.fwid)
 */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08003800, LENGTH = 702K  /* ends 0x080B3000: littlefs region (k2/drivers/flash.hpp) sits above the code */
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector ORIGIN(FLASH) :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .meta_version 0x08004000 :
  {
    . = ALIGN(4);
    KEEP(*(.meta_version))
  } > FLASH

  .meta_model 0x08004010 :
  {
    . = ALIGN(4);
    KEEP(*(.meta_model))
  } > FLASH

  .meta_commit 0x08004400 :
  {
    . = ALIGN(4);
    KEEP(*(.meta_commit))
  } > FLASH

  .text 0x08004404 :
  {
    . = ALIGN(4);
    KEEP(*(.fwid))          /* 48-byte "K2FW" version record at 0x08004404 (src/core/version.cpp) */
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  /* C++ static constructors: Reset_Handler walks these after .bss is zeroed, before main */
  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } > FLASH

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
    PROVIDE_HIDDEN (__init_array_end = .);
  } > FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    /* RAM-resident code (flash programming): runs while the flash bank is busy */
    *(.ramfunc)
    *(.ramfunc.*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  /* The .noinit canary of the Rd intent marker (src/core/rd_comeback.cpp): NOT zeroed at reset - Reset_Handler's zero loop
     starts at _sbss below - so it survives warm resets and whatever dark (VBUS loss) SRAM itself survives. NOLOAD: nothing is
     copied into it, its content at boot is whatever SRAM holds (the canary's magic+check say whether that is a live voucher).
     sysreset::wipe_and_reset (the DFU entry) zeroes all of SRAM, so a reflashed K2 never carries one. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(8);
    _snoinit = .;
    *(.noinit)
    *(.noinit*)
    . = ALIGN(8);
    _enoinit = .;
  } > RAM

  /* Zeroed RAM starts here: the USB-mode overlay first, then .bss (Reset_Handler zeroes both). */
  . = ALIGN(8);
  _sbss = .;
  __bss_start__ = _sbss;

  /* USB functions that never run in the same session share RAM (k2/drivers/mode_ram.hpp): Net and Drive
     are exclusive (usbcfg::normalize), each claims the region before touching it. Only buffers
     that nothing touches outside their own mode belong here: TinyUSB's ncm_interface and
     _mscd_itf are reset by tud_init() in every mode and stay in .bss.
     The .mode_net patterns match by object name under any build directory (build/, build-net/). A default
     build (K2_NET=0, Makefile) has none of those objects: .mode_net is empty and the overlay is as large as
     its next member, .mode_sniff. */
  OVERLAY : NOCROSSREFS
  {
    .mode_net
    {
      */lwip/*(.bss .bss.*)
      *ncm_device.o(.bss.ncm_epbuf)
      *dhserver.o(.bss .bss.*)
      */http_server.o(.bss .bss.*)
      */net.o(.bss .bss.*)
      . = ALIGN(8);
    }
    .mode_drive
    {
      *(.bss.mode_drive)
      . = ALIGN(8);
    }
    .mode_sniff
    {
      *(.bss.mode_sniff)
      . = ALIGN(8);
    }
    .mode_trans
    {
      *(.bss.mode_trans)
      . = ALIGN(8);
    }
  } > RAM

  .bss :
  {
    . = ALIGN(4);
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } > RAM

  /* No heap is used (nothing calls malloc). `end`/`_end` are kept for newlib's _sbrk reference.
     *
     * Stacks (nothing here is loaded or zeroed; Reset_Handler paints them):
     *   .stack_guard  0x80 bytes, 128-aligned, directly above .bss: an MPU no-access region (src/core/fault.cpp)
     *                 so a stack overflow faults instead of corrupting .bss
     *   PSP stack     _eguard.._psp_top: the application stack (main loop, UI, drivers); thread mode runs on PSP
     *                 and grows down from _psp_top
     *   .msp_stack    _psp_top.._estack: exceptions (ISRs, [FAULT] handler); MSP starts at _estack, so a fault
     *                 taken after the application stack overflowed still has a stack of its own
     * The gate (tools/gate.py) checks the PSP stack (_psp_top - _eguard) against the worst call depth and
     * .msp_stack against the deepest ISR / fault path. */
  PROVIDE ( end = _ebss );
  PROVIDE ( _end = _ebss );

  _msp_size = 1344;
  _psp_top = _estack - _msp_size;

  .stack_guard (NOLOAD) : ALIGN(128)
  {
    _sguard = .;
    . = . + 0x80;
    _eguard = .;
  } > RAM

  .msp_stack _psp_top (NOLOAD) :
  {
    . = . + _msp_size;
  } > RAM

  /* -fno-exceptions: discard ARM unwind tables that libgcc/newlib objects may emit. */
  /DISCARD/ :
  {
    *(.ARM.exidx*)
    *(.ARM.extab*)
  }

  ASSERT(_eguard <= _psp_top, "RAM overflow: .bss reaches the application stack")
  ASSERT(_psp_top + _msp_size == _estack, "MSP stack does not end at _estack")
}
