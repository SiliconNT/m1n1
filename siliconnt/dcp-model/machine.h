/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The machine the files under test run on (machine.c), as the M4 of SiliconNT's lab runs R44,
 * R44b and R44c has it: the display coprocessor (DCP) behind its mailbox, with the endpoint 0x23
 * of its iBoot interface in the V2 of AFK (SiliconNT docs/findings/0084) or in the V1, and the two
 * DARTs, with their page tables where iBoot left them. And the device tree (fake_adt.c).
 *
 * A case says what is different from that machine before it runs the code; afterwards it looks at
 * what was seen.
 */

#ifndef MACHINE_H
#define MACHINE_H

#include "types.h"

/* ---- the device tree ---- */

void fake_adt_reset(void);
/* A property added, or changed; NULL: taken away */
void fake_adt_prop(const char *path, const char *name, const void *value, u32 len);
void fake_adt_str(const char *path, const char *name, const char *value);
void fake_adt_reg(const char *path, int idx, u64 addr, u64 size);

/* ---- DCP ---- */

/* How far it gets, in the order things happen */
enum step {
    NEVER,
    AT_WAKE,        /* the wake-up: HELLO comes next */
    AT_HELLO_ACK,   /* the endpoint map comes next */
    AT_POWER,       /* the last ioreport message was answered: it is on */
    AT_INIT,        /* RBEP_INIT on endpoint 0x23: it wants a buffer */
    AT_GETBUF,      /* it has one: where the rings are */
    AT_START,       /* RBEP_START */
    AT_ANNOUNCE,    /* the endpoint is started: disp0-service */
    AT_OPEN,        /* the service is opened (V2) */
    AT_HPD,         /* is a display connected? */
    AT_SURFACE,     /* a surface */
    AT_SHUTDOWN,    /* RBEP_SHUTDOWN */
    AT_AP_QUIESCE,  /* the host is quiesced */
    AT_IOP_QUIESCE, /* DCP is to be */
    STEPS,
};
extern const char *const step_names[STEPS];

/* What it does from that step on, instead of answering */
enum way {
    ANSWERS,
    SILENT,       /* nothing at all */
    MUTE,         /* endpoint 0x23 says nothing, DCP is still there */
    CRASHES,      /* a crash, and then nothing */
    FLOODS_OTHER, /* its mailbox is never empty: messages of an endpoint nobody started */
    FLOODS_OWN,   /* ... of endpoint 0x23, of no known type */
    FLOODS_LOGS,  /* ... of the system log */
    FULL,         /* it takes no more messages */
    WAYS,
};
extern const char *const way_names[WAYS];

/* What goes into a ring of DCP's: an entry, its header as it should be unless said otherwise */
#define ENTRY_MAX 512
struct entry {
    u8 data[ENTRY_MAX];
    u32 len;
    u8 tail[16];   /* what was left there before, behind the entry */
    u32 bad_magic; /* 0: as it should be */
    bool is_bad_size;
    u32 bad_size;
};

#define ENTRIES_MAX 6
struct entries {
    struct entry e[ENTRIES_MAX];
    int n;
};
void entry_add(struct entries *list, const void *data, u32 len);
/* A message of the V2, with that behind its headers */
void entry_v2(struct entries *list, u16 iface, u8 type, u8 category, u8 flags, const void *body,
              u32 len);
/* A reply of the V2, with that behind the headers */
void entry_reply(struct entries *list, u16 iface, u8 flags, u8 tag, u32 status, const void *body,
                 u32 len);

#define SENT_MAX 32
struct machine {
    /* ---- the case ---- */
    enum way way;
    enum step from;
    bool v1;               /* endpoint 0x23: V1 of the rings and of the messages */
    u32 block;             /* of the rings */
    u32 bufsz[2];          /* in the headers of the rings, host to DCP first; 0: as it should be */
    u32 first_ptr;         /* of both rings, read and write */
    u16 ring_tag;          /* 0: as it should be */
    u32 ring_off, rb_size; /* where the second ring is, its size, in bytes */
    bool rings_before_buffer, recv_before_rings, only_tx_ring;
    struct entries announce, after_announce, hpd, surface;
    bool v1_no_replies;

    /* ---- what was seen ---- */
    struct {
        u8 ep;
        u64 msg;
    } ap[4096]; /* from the host */
    int n_ap;
    struct entry sent[SENT_MAX]; /* from the host, in the ring */
    int n_sent;
    int stray;       /* accesses to what is not there */
    int heap_errors; /* frees of what is not allocated */
    int asserts;     /* what DCP's firmware asserts on */
    int faults;      /* what DCP uses was not mapped while it was awake */
    int out_of_band; /* commands of that kind, V2 */
    int before_open; /* commands before the service was opened */
    int after_crash; /* messages to a DCP that has crashed */
    int surfaces;    /* that were set */
    u32 surface_fmt; /* of the last one */
    int crashes;
    int inits, shutdowns; /* RBEP_INIT, RBEP_SHUTDOWN */
    int fb_clears, fb_reinits;
    int surfaces_at_clear; /* how many surfaces were set when the screen was cleared */
    u64 opened_at, announced_at, surface_at, shutdown_at; /* microseconds */
    bool awake, dead;
    u32 iop_power, ap_power;
};
extern struct machine m;

/* Everything as on that M4, as iBoot leaves it, with nothing seen */
void machine_reset(void);

/* Messages of the V1 of the recorded kind: disp0-service announced */
void machine_v1(void);

enum outcome { RETURNED, HUNG, PANICKED };
/* Run it, until it returns, or has not returned for half a minute, or m1n1 gives up */
enum outcome machine_run(void (*fn)(void));
u64 machine_now(void);

/* DCP answers again, whatever way the case had it */
void machine_recover(void);
/* What DCP uses while it is awake is still there; m.faults if not */
void machine_check_buffers(void);
/* What the DARTs translate through is as iBoot left it */
bool machine_page_tables_untouched(void);
/* iBoot left a table of the second level with nothing in it, for the addresses m1n1 uses */
void machine_empty_l2(void);
/* The crash log, as 32-bit words from its first entry on */
void machine_crashlog(const u32 *words, int n);
/* Buffers for DCP that are allocated still */
int machine_dma_buffers(void);

bool printed(const char *text);
/* Every write, every message, everything printed goes to stdout */
extern bool transcript;
extern bool verbose;

#define FB_BASE   0x103e004c000UL
#define FB_DVA    0x1fc4000UL
#define FB_STRIDE 12096UL
#define FB_WIDTH  3024UL
#define FB_HEIGHT 1964UL

#endif
