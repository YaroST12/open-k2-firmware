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

/* The application flash region, for the memory readout (src/drivers/memstat.cpp): where it starts and where it ends. */
_flash_start = ORIGIN(FLASH);
_flash_end = ORIGIN(FLASH) + LENGTH(FLASH);

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

    /* Hot block: the curated set that must run with zero flash wait states. The AT32F403A flash is zero-wait
     * only below 0x08040000 (first 256 KB); the application is larger, so what lands there would otherwise be
     * decided by link order. Functions marked K2_HOT / tables marked K2_HOT_DATA (include/k2/core/hot.h)
     * come first, then the libc string helpers the hot paths call. `__hot_end` must stay <= 0x08040000;
     * tools/gate.py (step "hot") reports the size and the headroom and the ASSERT below stops the link. */
    __hot_start = .;
    /* K2_HOT_BLOCK_BEGIN: the Makefile drops everything up to the matching END marker for K2_HOT_BLOCK=0 builds */
    *(.text.hot .text.hot.*)
    /* TinyUSB is third-party C that cannot carry K2_HOT: pin its USB interrupt and transfer path by the
     * per-function section name (-ffunction-sections; clones keep the name plus a .constprop/.isra suffix). */
    *(.text.dcd_int_handler .text.dcd_int_handler.*)
    *(.text.dcd_event_handler .text.dcd_event_handler.*)
    *(.text.dcd_transmit_packet .text.dcd_transmit_packet.*)
    *(.text.handle_bus_reset .text.handle_bus_reset.*)
    *(.text.edpt_xfer .text.edpt_xfer.*)
    *(.text.usbd_edpt_xfer .text.usbd_edpt_xfer.*)
    *(.text.usbd_edpt_claim .text.usbd_edpt_claim.*)
    *(.text.tud_task_ext .text.tud_task_ext.*)
    *(.text.hidd_xfer_cb .text.hidd_xfer_cb.*)
    *(.text.hidd_control_xfer_cb .text.hidd_control_xfer_cb.*)
    *(.text.cdcd_xfer_cb .text.cdcd_xfer_cb.*)
    *(.text.cdcd_control_xfer_cb .text.cdcd_control_xfer_cb.*)
    *(.text.tud_control_xfer .text.tud_control_xfer.*)
    *(.text.tud_control_status .text.tud_control_status.*)
    *(.text.tu_fifo_read_n_access_mode .text.tu_fifo_read_n_access_mode.*)
    *(.text.tu_fifo_write_n_access_mode .text.tu_fifo_write_n_access_mode.*)
    *(.text.tu_edpt_stream_write .text.tu_edpt_stream_write.*)
    *(.text.tu_edpt_stream_write_xfer .text.tu_edpt_stream_write_xfer.*)
    *(.text.tu_edpt_stream_read_xfer .text.tu_edpt_stream_read_xfer.*)
    *(.text.tu_edpt_stream_write_available .text.tu_edpt_stream_write_available.*)
    /* Header-inline hot helpers (include/k2/ui/blend.hpp, readout_fmt.hpp, smoothing.hpp): under LTO the
     * K2_HOT section attribute is dropped from an inline (COMDAT) function that is emitted out of line,
     * so these are pinned by their mangled -ffunction-sections names instead. */
    *(.text._ZN2k28blend565E* .text._ZN2k214make_blend_lutE*)
    *(.text._ZN2k27readout* .text._ZZN2k27readout*)
    *(.text._ZN2k29smoothing* .text._ZZN2k29smoothing*)
    /* pdtrace ring / collapse / capture templates (include/k2/pd/pdtrace.hpp): same COMDAT case, on the PD path */
    *(.text._ZNK2k27pdtrace4Ring* .text._ZN2k27pdtrace4Ring* .text._ZN2k27pdtrace8Collapse* .text._ZN2k27pdtrace8Captures*)
    /* 64-bit division slow path (include/k2/core/math_opt.hpp): inline + noinline, so a COMDAT section that cannot carry K2_HOT */
    *(.text._ZN2k24math*)
    /* Other header-inline helpers that run per PD message or per paint (caps_log_gate.hpp, pd_interrogate.hpp, lcd.hpp,
     * lcd.cpp PaintTimer) and the TinyUSB FIFO copy loops the hidd/cdcd transfer path calls */
    *(.text._ZN2k22pd18caps_content_hash* .text._ZNK2k22pd11Interrogate7running*)
    *(.text._ZN2k23lcd12clip_to_band* .text._ZN2k23lcd12_GLOBAL__N_110PaintTimer*)
    *(.text._ZN2k27cable_r8fmt_mohm* .text._ZZN2k27cable_r8fmt_mohm*)
    /* K2_HOT header-inline classes and the small out-of-line helpers of the PD decoders, the power average, the scratch
     * lease and string compares (selftest gfx_fill, pd_codec, pd_sink, power_calc reach them per call) */
    *(.text._ZN2k29PixelSink* .text._ZNK2k29PixelSink* .text._ZN2k25power7Average* .text._ZNK2k25power7Average*)
    *(.text._ZN2k22pd12_GLOBAL__N_14le32* .text._ZN2k27scratch7acquire* .text._ZN2k27scratch5arena*)
    *(.text._ZSt3minIjET_St16initializer_listIS0_E* .text._ZSt6copy_nIN9__gnu_cxx17__normal_iteratorIPKmSt4span*)
    *(.text._ZNSt7__equalILb1EE5equalIcEEbPKT_S4_S4_*)
    *(.text.tu_hwfifo_write .text.tu_hwfifo_write.* .text.tu_hwfifo_read .text.tu_hwfifo_read.*)
    /* QR encoder (src/ui/qr.cpp over the vendored generator): the share page encodes a version 13..21 symbol in one
     * blocking call, the largest compute block on a user path (selftest qr_encode). The generator is third-party C,
     * so its entry point, its static helpers (as named by -ffunction-sections, with any .constprop/.isra suffix) and
     * its two block tables are pinned by section name. Without the pin the helpers land in the window by link order
     * and the entry point past it. */
    *(.text.qrcodegen_* .text._ZN2k22qr13encode_placed*)
    *(.text.appendBitsToBuffer .text.appendBitsToBuffer.* .text.addEccAndInterleave .text.addEccAndInterleave.*)
    *(.text.getNumDataCodewords .text.getNumDataCodewords.* .text.getNumRawDataModules .text.getNumRawDataModules.*)
    *(.text.calcReedSolomonGenerator .text.calcReedSolomonGenerator.* .text.calcReedSolomonRemainder .text.calcReedSolomonRemainder.*)
    *(.text.finiteFieldMultiply .text.finiteFieldMultiply.* .text.initializeFunctionModules .text.initializeFunctionModules.*)
    *(.text.drawWhiteFunctionModules .text.drawWhiteFunctionModules.* .text.drawFormatBits .text.drawFormatBits.*)
    *(.text.getAlignmentPatternPositions .text.getAlignmentPatternPositions.* .text.fillRectangle .text.fillRectangle.*)
    *(.text.drawCodewords .text.drawCodewords.* .text.applyMask .text.applyMask.*)
    *(.text.getPenaltyScore .text.getPenaltyScore.* .text.addRunToHistory .text.addRunToHistory.*)
    *(.text.hasFinderLikePattern .text.hasFinderLikePattern.* .text.getModule .text.getModule.*)
    *(.text.setModule .text.setModule.* .text.setModuleBounded .text.setModuleBounded.*)
    *(.text.getBit .text.getBit.* .text.calcSegmentBitLength .text.calcSegmentBitLength.*)
    *(.text.getTotalBits .text.getTotalBits.* .text.numCharCountBits .text.numCharCountBits.*)
    *(.rodata.ECC_CODEWORDS_PER_BLOCK .rodata.ECC_CODEWORDS_PER_BLOCK.* .rodata.NUM_ERROR_CORRECTION_BLOCKS .rodata.NUM_ERROR_CORRECTION_BLOCKS.*)
    /* Hot entry points of mostly-cold modules (keeps cold helpers under -Oz COLD_SIZE_FLAGS) */
    *(.text._ZNK2k22pd11Interrogate11sopp_window* .text._ZNK2k22pd11Interrogate7settled* .text._ZN2k22pd11Interrogate7on_sopp*)
    *(.text._ZN2k28transcap4pollEv* .text._ZN2k28transcap6sampleEv* .text._ZN2k28transcap12_GLOBAL__N_19poll_onceEv*)
    *(.text._ZN2k29bbpersist4poll* .text._ZN2k26rescan4pollEv*)
    /* UI fonts and 12x12 icon table (zero-wait reads speed up glyph decode and badge paints) */
    *(.rodata._ZN2k29kIconBitsE*)
    *(.rodata._ZN2k27font_12E* .rodata._ZN2k27font_16E* .rodata._ZN2k27font_24E* .rodata._ZN2k27font_48E* .rodata._ZN2k27font_64E*)
    *(.rodata._ZN2k212_GLOBAL__N_1L6bitmapE* .rodata._ZN2k212_GLOBAL__N_1L6glyphsE*)
    /* Real-time PD link/limiter/passive, compression, sweep, formatting, DSP, and UI kit primitives. A lambda's section
     * is its enclosing function's name behind _ZZ (_ZZN / _ZZNK) and does not inherit the pin: each prefix is pinned
     * in both forms (gate step "hot" lists lambdas of hot-block functions that land past the window). */
    *(.text._ZN2k22pd4Link* .text._ZNK2k22pd4Link* .text._ZN2k22pd7Passive* .text._ZNK2k22pd7Passive*)
    *(.text._ZZN2k22pd4Link* .text._ZZNK2k22pd4Link* .text._ZZN2k22pd7Passive* .text._ZZNK2k22pd7Passive*)
    *(.text._ZN2k22pd7Limiter* .text._ZNK2k22pd7Limiter* .text._ZZN2k22pd7Limiter* .text._ZZNK2k22pd7Limiter*)
    *(.text._ZN2k22lz* .text._ZZN2k22lz* .text._ZN2k27ctsweep* .text._ZNK2k27ctsweep* .text._ZZN2k27ctsweep* .text._ZZNK2k27ctsweep*)
    /* Cold-file callees of the sweep line and the PD decoders (selftest sweep_sum, pd_codec): the packed-string reader with
     * its pair table, the vendor and SVID names of a parsed e-marker */
    *(.text._ZN2k27strpack* .text._ZNK2k27strpack* .text._ZZN2k27strpack* .rodata._ZN2k27strpack5table5kPairE*)
    *(.text._ZN2k22pd11vendor_name* .text._ZN2k22pd9svid_name* .text._ZN2k22pd10VendorNameC*)
    *(.text._ZN2k210StrBufBase* .text._ZN2k23mixEmSt17basic_string_view* .text._ZN2k22pg8mix_cstr* .text._ZN2k22pg8mix_u32s* .text._ZN2k27fmt_fit* .text._ZN2k27fmt_u32* .text._ZN2k29fmt_fixed* .text._ZN2k213fmt_abs_fixed*)
    *(.text._ZN2k210fastcharge* .text._ZN2k27dpsniff* .text._ZN2k26fctrig4poll* .text._ZN2k27battery* .text._ZN2k26ripple*)
    *(.text._ZZN2k210fastcharge* .text._ZZN2k27dpsniff* .text._ZZN2k26fctrig4poll* .text._ZZN2k27battery* .text._ZZN2k26ripple*)
    *(.text._ZN2k23lcd* .text._ZN2k23kit* .text._ZNK2k23kit* .text._ZZN2k23lcd* .text._ZZN2k23kit* .text._ZZNK2k23kit*)
    /* Live screen update/paint paths and bezel layout helpers */
    *(.text._ZN2k213opp_slot_rect* .text._ZN2k213panel_text_dir* .text._ZN2k212opp_reversed* .text._ZN2k212_GLOBAL__N_115build_opp_cells*)
    *(.text._ZN2k28HomePage* .text._ZNK2k28HomePage* .text._ZN2k24home* .text._ZZN2k28HomePage* .text._ZZNK2k28HomePage* .text._ZZN2k24home*)
    *(.text._ZN2k214ConnectionPage* .text._ZNK2k214ConnectionPage* .text._ZZN2k214ConnectionPage* .text._ZZNK2k214ConnectionPage*)
    *(.text._ZN2k210DetailPage* .text._ZNK2k210DetailPage* .text._ZN2k29LinesPage* .text._ZNK2k29LinesPage* .text._ZN2k210RipplePage* .text._ZNK2k210RipplePage* .text._ZN2k210GraphsPage* .text._ZNK2k210GraphsPage* .text._ZN2k211BatteryPage* .text._ZNK2k211BatteryPage*)
    *(.text._ZZN2k210DetailPage* .text._ZZNK2k210DetailPage* .text._ZZN2k29LinesPage* .text._ZZNK2k29LinesPage* .text._ZZN2k210RipplePage* .text._ZZNK2k210RipplePage* .text._ZZN2k210GraphsPage* .text._ZZNK2k210GraphsPage* .text._ZZN2k211BatteryPage* .text._ZZNK2k211BatteryPage*)
    *(.text._ZN2k215ChargerTestPage13paint_running* .text._ZZN2k215ChargerTestPage13paint_running*)
    *(.text._ZNK2k210StackArenaINS_6PageIfE*2atEj*)
    *libc.a:libc_a-memcpy.o(.text .text.*)
    *libc.a:libc_a-memmove.o(.text .text.*)
    *libc.a:libc_a-memset.o(.text .text.*)
    *(.text.strlen .text.strlen.*)
    *libc.a:libc_a-strlen.o(.text .text.*)
    *(.text.strcmp .text.strcmp.* .text.memcmp .text.memcmp.*)
    *(.rodata.hot .rodata.hot.*)
    /* K2_HOT_BLOCK_END */
    . = ALIGN(4);
    __hot_end = .;

    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    __start_k2_strpack = .;
    *(k2_strpack k2_strpack.*)
    __stop_k2_strpack = .;
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    /* Placement canary (src/core/math_selftest.cpp, ws_slow): one function pinned at the far end of .text so it
     * always runs from the wait-state region; the ASSERT below keeps it past the zero-wait window. */
    . = ALIGN(4);
    __cold_probe_start = .;
    KEEP(*(.cold_probe))
    KEEP(*(.cold_probe.data))   /* the data-read canary's table (math_selftest.cpp, rd_slow) */
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

  ASSERT(__hot_end <= 0x08040000, "hot block (K2_HOT) overflows the zero-wait flash window (0x08000000..0x0803FFFF)")
  ASSERT(__cold_probe_start >= 0x08040000, "cold probe (.cold_probe) landed inside the zero-wait window: the placement canary would measure nothing")
  ASSERT(_eguard <= _psp_top, "RAM overflow: .bss reaches the application stack")
  ASSERT(_psp_top + _msp_size == _estack, "MSP stack does not end at _estack")
}
