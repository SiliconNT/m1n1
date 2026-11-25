/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * sgitest.ld for xtstress: the page that is executed and written to at the same time (.hot) has
 * 16 KB to itself, and what is written to otherwise starts on a 16 KB page of its own, so that
 * no other page of this image is both executed and written to. 16 KB covers a page of execution
 * tracking whether the hypervisor tracks 4 KB or 16 KB.
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

    . = ALIGN(0x4000);

    .hot : {
        *(.hot)
        . = ALIGN(0x4000);
    }

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
