/* SPDX-License-Identifier: MIT */

#ifndef HV_XHCI_H
#define HV_XHCI_H

#include "types.h"

/*
 * Emulated xHCI controller with one high-speed USB HID device (boot keyboard + boot mouse)
 * for the standalone hypervisor guest. Guest-visible MMIO window and interrupt:
 */
#define HV_XHCI_BASE 0x5300000000ULL
#define HV_XHCI_SIZE 0x10000
#define HV_XHCI_IRQ  999 /* GICv3 SPI INTID; vGIC unmasks the same AIC IRQ, so pick one no device uses */

/* Interfaces of the emulated device */
#define HV_USBHID_KBD   0
#define HV_USBHID_MOUSE 1

int hv_xhci_init(u64 guest_base, u64 guest_end);
/* Called from the hypervisor tick (boot CPU): poll inputs, complete pending transfers, interrupt */
void hv_xhci_tick(void);
/* Print controller/device state on the console (debug overlay) */
void hv_xhci_dump(void);
/* Queue an input report for one of the interfaces (boot keyboard: 8 bytes, boot mouse: 4 bytes) */
void hv_usbhid_push(int iface, const u8 *report, u32 len);
/* Keyboard LED state last set by the guest (SET_REPORT), bit 1 = caps lock */
u8 hv_usbhid_leds(void);

#endif
