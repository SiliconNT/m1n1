/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see run_test.c */

#ifndef IODEV_H
#define IODEV_H

#define USB_IODEV_COUNT 8

/* As in src/iodev.h */
typedef enum _iodev_id_t {
    IODEV_UART,
    IODEV_DOCKCHANNEL_UART,
    IODEV_FB,
    IODEV_USB_VUART,
    IODEV_USB0,
    IODEV_MAX = IODEV_USB0 + USB_IODEV_COUNT,
    IODEV_LOG = IODEV_MAX,
    IODEV_NUM,
} iodev_id_t;

typedef int iodev_usage_t;
void iodev_set_usage(iodev_id_t id, iodev_usage_t usage);
iodev_usage_t iodev_get_usage(iodev_id_t id);
#endif
