/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * sgitest is loaded as a raw image at an address it does not know, so everything, including
 * what would be .bss, has to be in the file. The trailing word keeps the zeroes in.
 */

ENTRY(image_start)

SECTIONS
{
    . = 0;

    .text : {
        *(.head)
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : {
        . = ALIGN(16);
        *(.data .data.*)
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        QUAD(0x444e454954534753) /* "SGSTIEND" */
    }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
