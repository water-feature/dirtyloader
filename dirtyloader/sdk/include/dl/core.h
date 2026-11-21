/* SPDX-License-Identifier: GPL-2.0-or-later */
/* core's API for loadable mods (core 0.4 and later): what core does for a mod beyond calling its handlers.
 * The M8 counterpart of core-dn1's exports (core_restart, the Mod Menu and mod pages, project data, the
 * +Drive and MIDI routines). docs/LOADER.md, "The core API".
 *
 *     #include "dl/mod.h"
 *     #include "dl/core.h"
 *     #include "m8-headless/fw.h"            (DL_CORE_PTR)
 *
 *     static void boot(void) {
 *         const struct dl_core *c = dl_core();
 *         if (c) c->page_add(&my_page);
 *     }
 *
 * dl_core() is 0 under a core without the API: check it once, at ev_boot. Call it from the UI thread (ev_boot,
 * ev_tick, ev_key, ev_draw, a page's callbacks), never from the audio thread, except midi_send. */
#ifndef DIRTYLOADER_DL_CORE_H
#define DIRTYLOADER_DL_CORE_H

#include <stddef.h>
#include <stdint.h>

#define DL_CORE_MAGIC   0x34434C44u      /* "DLC4" */
#define DL_CORE_SD_MAGIC 0x53434C44u     /* "DLCS": SD-only core */
#define DL_CORE_VERSION 0x00000600u      /* 0.6: mod_dest_of; mod_dest_add past 16 */

/* A screen of a mod's own (core-dn1's Mod Menu entry and mod page in one). Core shows it in the firmware's free
 * view slot: from the MODS page (RIGHT on the mod's row, which shows ">" when it has a page), or when the mod
 * calls page_open. Core clears the screen when the page opens; then draw() runs every frame: write rows with
 * fw_text(fw_grid, col, row, text). key() gets each key chord (FW_KEY_*); return nonzero to keep it, 0 to let
 * core have it (OPT closes the page). */
struct dl_page {
    const char *mod;             /* the mod's id (dl_mod.id): which MODS row opens it */
    const char *title;           /* drawn on row 3 when the page opens */
    void (*open)(void);          /* may be 0 */
    void (*draw)(void);
    int  (*key)(int action);     /* may be 0 */
};

/* A block of a mod's data kept with each song (core-dn1's project data): saved beside the song as
 * <song>.dldata when the song is saved, put back when it is loaded (the boot's AUTOSAVE included). loaded(n)
 * says how many bytes came back: 0 when the song has none (a new song, or saved without the mod): reset to the
 * mod's defaults then. */
struct dl_song_data {
    const char *id;              /* up to 23 characters, unique across mods: the mod's id, say */
    void *data;
    uint32_t size;
    void (*loaded)(uint32_t n);  /* may be 0 */
};

struct dl_setting {
    const char *label;           /* up to 16 characters, unique: the file keeps values by it */
    uint8_t *value;
    uint8_t min, max;
    const char *const *names;    /* names[value - min] to show, or 0 for the number */
    void (*changed)(void);       /* may be 0 */
};

struct dl_core {
    uint32_t magic;              /* DL_CORE_MAGIC */
    uint32_t version;            /* DL_CORE_VERSION or later; later versions only add members at the end */
    uint32_t size;               /* sizeof(struct dl_core) of the running core */

    void (*restart)(void);                                   /* reboot now (the mods load again) */

    int  (*page_add)(const struct dl_page *p);              /* 1, or 0 when core's table (8) is full */
    void (*page_open)(const struct dl_page *p);             /* show it now (from ev_key, say) */
    void (*page_close)(void);                                /* back to the screen it was opened over */

    int  (*song_data)(const struct dl_song_data *d);        /* register at ev_boot: 1, or 0 when full (8) */

    /* Files on the SD card, by absolute path. The UI thread only, and not while the firmware is saving or
     * loading (ev_song_save / ev_song_load just ran): it shares the card. -1 on error. */
    int  (*file_read)(const char *path, uint32_t offset, void *buf, uint32_t n);    /* bytes read */
    int  (*file_write)(const char *path, const void *buf, uint32_t n);              /* creates or replaces */

    void (*midi_receive)(uint32_t packet);   /* play a USB MIDI packet into the M8 as if it came from the host */
    void (*midi_send)(uint32_t packet);      /* send a USB MIDI packet to the host (ev_midi_out sees it) */

    const volatile uint32_t *last_crash;     /* Teensyduino's crash record the boot found ([6] is the pc), or 0 */
    const char *song;                        /* the song last loaded or saved ("" before any) */

    /* 0.5 (check version >= 0x500): an instrument file into a slot (0x00-0x7F), or a slot to a file, as the
       instrument screen's LOAD / SAVE do (ev_inst_load / ev_inst_save see them). Nonzero on success. UI thread. */
    int (*inst_load)(const char *path, int slot);
    int (*inst_save)(const char *path, int slot);

    /* 0.5: a modulation destination added to an engine's list (FW_ENGINE_*): it shows in the instrument MODS
       page's DEST and can be picked like a stock one; ev_mod_apply gets its index as `dest` (the stock code does
       nothing with it). Returns that index, or 0 when the engine's list is full (16, the storage's limit; 0.6:
       31 where extended destinations exist, and indices from 15 come through mod_dest_of). Call at ev_boot; the
       name must stay valid. */
    int (*mod_dest_add)(int engine, const char *name);

    /* 0.5: the Mod Menu (core-dn1's): a screen with every mod page as a grid, and below it the mods' settings
       rows (core-dn1's core_additem). Opened from the MODS page's MOD MENU row, or by a mod with menu_open (from
       its own ev_key handler, say). A setting is a byte between min and max that LEFT / RIGHT change; core keeps
       it in /dirtyloader/settings.txt by label and puts it back when setting_add is called (at ev_boot). */
    int  (*setting_add)(const struct dl_setting *s);        /* 1, or 0 when full (8) */
    void (*menu_open)(void);

    /* 0.6 (check version >= 0x600): extended destinations. On firmware that has them (headless 6.6.4A),
       mod_dest_add takes up to 16 more per engine, past the storage's 4 bits: a modulator on one keeps 15 in its
       packed byte and core keeps the real index (docs/MOD-DESTS.md). In ev_mod_apply, mod_dest_of(modulator,
       dest) gives the real index; it returns `dest` itself for every other modulator. Audio thread safe. */
    int (*mod_dest_of)(void *modulator, int dest);

    /* 0.6: a buffer from RAM the firmware never uses (headless: 40 KB of DTCM between .bss and the stack), 8-byte
       aligned, for as long as the unit runs; 0 when there is not that much left, or none on this firmware. Call
       at ev_boot. Not zeroed. */
    void *(*ram_claim)(uint32_t bytes);
};

/* USB MIDI packets: byte 0 is cable << 4 | CIN (the status's high nibble for channel messages), then the bytes */
#define DL_MIDI_PACKET(status, d1, d2) \
    ((uint32_t)((status) >> 4 & 0xF) | (uint32_t)(status) << 8 | (uint32_t)(d1) << 16 | (uint32_t)(d2) << 24)
#define DL_MIDI_STATUS(p) ((uint8_t)((p) >> 8))
#define DL_MIDI_D1(p)     ((uint8_t)((p) >> 16))
#define DL_MIDI_D2(p)     ((uint8_t)((p) >> 24))

#ifdef DL_CORE_PTR
static inline const struct dl_core *dl_core_table(void)
{
    const struct dl_core *c = *(const struct dl_core *const volatile *)DL_CORE_PTR;
    uint32_t a = (uint32_t)c;
    if (a < 0x60000000u || a >= 0x60800000u || (a & 3)) return 0;   /* core's API lives in its flash */
    return c;
}

static inline const struct dl_core *dl_core(void)
{
    const struct dl_core *c = dl_core_table();
    return c && c->magic == DL_CORE_MAGIC ? c : 0;
}

/* Write from the UI thread. Also available on the SD-only core.
 * Returns bytes written, or -1 if unavailable or the write failed. */
static inline int dl_core_file_write(const char *path, const void *buf, uint32_t n)
{
    const struct dl_core *c = dl_core_table();
    if (!c || (c->magic != DL_CORE_MAGIC && c->magic != DL_CORE_SD_MAGIC) ||
        c->size < offsetof(struct dl_core, file_write) + sizeof(c->file_write) || !c->file_write)
        return -1;
    return c->file_write(path, buf, n);
}
#endif

#endif
