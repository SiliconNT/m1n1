/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * sgitest.ld for xitest: the same, but what is written to starts on a page of its own, so
 * that no page of this image is both executed and written to.
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

    . = ALIGN(4096);

    .data : {
        . = ALIGN(16);
        *(.data .data.*)
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        QUAD(0x444e454954534753) /* "SGSTIEND" */
        image_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
