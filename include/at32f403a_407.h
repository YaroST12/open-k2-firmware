// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
/* Minimal stand-in for Artery's at32f403a_407.h (CMSIS device header).
 *
 * TinyUSB's fsdev_at32.h includes this header for four things only:
 * APB1PERIPH_BASE, IRQn_Type, NVIC_EnableIRQ/NVIC_DisableIRQ and the
 * CRM->intmap_bit.usbintmap flag. This tree has no vendor SDK, so those are
 * provided here instead of importing ~MB of BSP headers.
 *
 * The K2 vector table (src/startup.c) wires the shared USB/CAN vectors
 * (IRQ 19, 20) and USBWakeUp (IRQ 42). The USB interrupt remap
 * (CRM INTMAP.USBINTMAP) is never enabled, so the remap IRQs below are never
 * reached; usbintmap is a constant 0.
 */
#ifndef AT32F403A_407_SHIM_H
#define AT32F403A_407_SHIM_H

#include <stdint.h>

#define APB1PERIPH_BASE 0x40000000UL

typedef enum {
    USBFS_H_CAN1_TX_IRQn  = 19,
    USBFS_L_CAN1_RX0_IRQn = 20,
    USBFSWakeUp_IRQn      = 42,
    /* Remapped USB IRQs: unused, see above. */
    USBFS_MAPH_IRQn       = 19,
    USBFS_MAPL_IRQn       = 20,
} IRQn_Type;

#define K2_NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100UL + 4UL * (uint32_t)(n)))
#define K2_NVIC_ICER(n) (*(volatile uint32_t *)(0xE000E180UL + 4UL * (uint32_t)(n)))

static inline void NVIC_EnableIRQ(IRQn_Type irq) {
    K2_NVIC_ISER((uint32_t)irq >> 5) = 1UL << ((uint32_t)irq & 31U);
}

static inline void NVIC_DisableIRQ(IRQn_Type irq) {
    K2_NVIC_ICER((uint32_t)irq >> 5) = 1UL << ((uint32_t)irq & 31U);
}

typedef struct {
    struct { uint32_t usbintmap; } intmap_bit;
} k2_crm_shim_t;

static const k2_crm_shim_t k2_crm_shim = { { 0 } };
#define CRM (&k2_crm_shim)

#endif /* AT32F403A_407_SHIM_H */
