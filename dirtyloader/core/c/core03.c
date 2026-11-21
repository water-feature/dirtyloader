/* SPDX-License-Identifier: GPL-2.0-or-later */
/* core 0.4: load mods from the SD card at boot, and serve them (docs/LOADER.md).
 *
 * Runs from flash past the end of the stock image and keeps all of its state in the free ITCM tail, so it has
 * no .data or .bss of its own. Three entry points, all called from stubs the builder places:
 *
 *   core_early()  first thing in main(), before setup(): make ITCM writable, retire the last boot's table
 *   core_load()   inside setup(), just after the card is mounted and before the firmware touches it (no
 *                 autosave yet, no samples, no audio): crash guard, load the mods in active.txt, status.txt
 *   core_boot()   after setup(), before ev_boot: switch the dispatcher to the table in ITCM
 *
 * Every SD access is in core_load, before the firmware itself uses the card, so core never has a file open
 * while the firmware does. The crash guard needs no file write later on either: Teensy's crash record and
 * this RAM survive the reboot after a crash, so core_load can tell that the last boot with mods crashed.
 *
 * Built for one firmware: CORE_FW_H names its header (m8-headless/fw.h). */
#include <stdint.h>
#include "dl/mod.h"
#include CORE_FW_H
#include "dl/core.h"

/* ---- layout, shared with dirtyloader/core/builder.py (build_core03_mod) -------------------------------- */
#ifndef CORE_RAM
#error "CORE_RAM, CORE_RAM_END, CORE_FLASH_TABLE, CORE_FLASH_SLOTS come from the builder"
#endif
#define RAM_MAGIC    0x33524C44u       /* "DLR3": the dispatcher uses ram->table only when this is set */
#define RAM_SIG      0x47495344u       /* "DSIG": ram->nmods is from a previous boot of this core */
#define SLOTS        64
#define MAX_MODS     16
#define NAME_MAX     28                /* an .m8x file name the MODS page and the boot record keep */
#define FLAG_ENABLED 1u
#define MAX_PAGES    8
#define MAX_DATA     8
#define MIDI_Q       8
#define MAX_SETTINGS 24
#ifdef CORE_SD_ONLY
/* The boot-only core reserves no engine-specific capacity. */
#define FW_ENGINES 1
#define FW_MOD_DESTS_MAX 1
#endif
#ifdef FW_MODULATOR_FIRST
#define DEST_EXT 1                     /* the header has what extended destinations need (core 0.6) */
#define DEST_EXTRA_MAX 16              /* destinations a mod may add to an engine: up to 31 in all */
#else
#define DEST_EXT 0
#define DEST_EXTRA_MAX FW_MOD_DESTS_MAX
#endif
#define DEST_MARK 15u                  /* the packed byte's dest for "extended": the real one is in dests.ext */
#define DEST_MISSING 0xFFu             /* dests.ext: the song names a destination no loaded mod adds */

struct hook { uint16_t event, flags; void *fn; };

struct core_ram {
    uint32_t magic;
    uint32_t count;
    struct hook table[SLOTS];
    const struct dl_core *api;   /* +0x208, DL_CORE_PTR: mods find core's API here */
    uint32_t sig;                /* RAM_SIG once core_load has run; ITCM survives a reset, not a power cycle */
    uint32_t loaded;             /* core_load ran this boot */
    uint32_t next;               /* next free byte of the mods' area */
    uint32_t nmods;
    const struct dl_mod *mods[MAX_MODS];
    /* what core_load did with each line of active.txt, for the MODS page */
    uint32_t nboot;
    struct { char name[NAME_MAX]; int32_t err; } boot[MAX_MODS];
    /* the MODS page (TELOS): a screen in the firmware's free view slot */
    struct { const void *const *vt; uint8_t body[0x3C]; } page_view;
    uint32_t page_n, page_sel, page_dirty, last_action;
    struct { char name[NAME_MAX]; uint8_t on, check; } page[MAX_MODS];   /* check: E<n> found when switching on */
    /* songs and their mods: core_tick writes / checks <song>.mods after the firmware's own save / load */
    uint32_t booted, song_pending, page_mode, prev_view, nwant, song_result, song_at, song_tries;
    uint32_t audio_orig;           /* the audio interrupt core_audio wraps */
    uint32_t skipped;              /* the crash guard skipped the mods this boot */
    char song[128];
    struct { char name[NAME_MAX]; } want[MAX_MODS];
    /* core 0.4: mods' pages and song data, MIDI played in by mods, the crash the boot found */
    const struct dl_page *pages[MAX_PAGES];
    uint32_t npages;
    const struct dl_page *cur_page;
    uint32_t page_back;            /* the view a mod page was opened over; PAGE_BACK_MODS: the MODS page */
    const struct dl_song_data *data[MAX_DATA];
    uint32_t ndata, data_done;
    uint32_t midi_q[MIDI_Q];
    uint32_t midi_head, midi_tail;
    uint32_t guard_recovery, reserved_guard;   /* keep retained crash fields at the same offsets */
#ifdef CORE_GUARD_DIAGNOSTICS
    uint32_t reserved_guard_diagnostics[2];
#endif
    uint32_t crash[8];             /* Teensyduino's crash record as the boot found it; [0] 0: none */
    uint32_t fault_orig, in_fault, fault_handled, fault_pc;   /* ev_crash; the last two survive the reset */
    uint32_t dest_name_orig[FW_ENGINES], dest_count_orig[FW_ENGINES], dest_stock[FW_ENGINES], dest_n[FW_ENGINES];
    const char *dest_extra[FW_ENGINES][DEST_EXTRA_MAX];
    uint32_t tap_orig, audio_subs;
    uint32_t track_orig, mfx_orig, delay_orig, reverb_orig, clock_orig, track_subs, fx_subs, params_subs;   /* the tap's update() core_tap wraps; ev_audio_out has handlers */
    uint32_t sysex_orig;           /* the firmware's SysEx handler core_sysex hands chunks on to */
    const struct dl_setting *settings[MAX_SETTINGS];
    uint32_t nsettings, menu_sel, menu_back;
    uint32_t bulk_orig, cell_orig, snap_phrase;   /* ev_step_edit's bulk reports: the phrase as last seen */
    uint8_t snap[16 * 9];
    /* core 0.6, extended destinations: a modulator whose packed byte holds DEST_MARK has its real destination
       here, by instrument and modulator (0: none, read as DEST_MARK). Kept with the song (core:dests). */
    struct {
        uint8_t ext[128][4];
        uint16_t hash[FW_ENGINES][DEST_EXTRA_MAX];   /* at save: each added destination's name, hashed */
    } dests;
    uint8_t dest_clip;             /* the modulator copied on INST. MODS (SHIFT+OPT): its real destination */
    uint8_t dest_load_slot, dest_load_ok;
    uint16_t dest_load[4];         /* ev_inst_load's sidecar (name hashes), resolved once the instrument is in */
    uint32_t usb_last;             /* usb_configuration as ev_usb last reported it; 0xFF before the first */
    uint32_t spare_next;           /* ram_claim's next free byte in the firmware's spare RAM */
    /* core 0.7: mods that don't fit in ITCM run their code from flash (FW_MOD_FLASH) */
    uint32_t flash_next;           /* the next free 4 KB of it */
    uint32_t last_flash, last_flash_n, last_written;   /* the mod load_one just placed there; 0: in ITCM */
    /* core 0.7: each mod's code (its image in ITCM, or its code in flash): which mod a setting belongs to */
    uint32_t mod_lo[MAX_MODS], mod_hi[MAX_MODS];
    /* the machine pages (LEFT+RIGHT): a page per mod, its settings as knobs */
    uint32_t mp_page, mp_sel, mp_prev, mp_back, mp_gfx;
    uint16_t mp_drawn[8];          /* each knob's value as drawn, | 0x100 selected, | 0x8000 drawn at all */
    /* the MOD LOADER (Model:02, all four directions): cards of the mods on the card */
    uint32_t lm_sel, lm_tab, lm_prev, lm_back, lm_dirty, lm_wait, lm_shown_sel, lm_page, mp_dirty;
    /* the extended theme: the draw loop's palette (the firmware's 13, mirrored, then 15 of core's) */
    uint32_t xt_pal[28];
    uint8_t xt_rgb[45], xt_flags, xt_on, xt_rsv[2];
#ifdef fw_rect
    /* the MOD LOADER's selected icon: its animation frame and when it last moved; core_tick opens the visualizer
       for a card that asked (EDIT on CHAOS CONTROL); the footer as drawn (the selected mod's ON / OFF) */
    uint32_t lm_anim, lm_anim_at, lm_vis, lm_foot_open;
    uint32_t lm_mode;              /* LM_CARDS, LM_INFO (a mod's INFO page), LM_ASK (RESTART TO APPLY?) */
#endif
};
_Static_assert(__builtin_offsetof(struct core_ram, api) == 0x208, "dl/core.h: DL_CORE_PTR is the RAM + 0x208");
#ifdef DL_CORE_PTR
_Static_assert(DL_CORE_PTR == CORE_RAM + 0x208, "the device header's DL_CORE_PTR is not this core's");
#endif
#define PAGE_BACK_MODS 0xFFu
#define PAGE_BACK_MENU 0xFEu
#define PAGE_BACK_LOADER 0xFDu
#define ram ((struct core_ram *)CORE_RAM)
#define AREA_START ((CORE_RAM + sizeof(struct core_ram) + 7u) & ~7u)
/* boot-time scratch for status.txt and active.txt at the top of ITCM, off the main stack */
#define STATUS_SIZE 1024u
#define LIST_SIZE 512u
#define SCRATCH (CORE_RAM_END - STATUS_SIZE - LIST_SIZE)
#define AREA_END SCRATCH

/* ---- small libc ------------------------------------------------------------------------------------------ */
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void zero(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
__attribute__((unused)) static void copy(void *d, const void *s, uint32_t n) { uint8_t *o = d; const uint8_t *i = s; while (n--) *o++ = *i++; }
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *put_hex(char *o, uint32_t v)
{
    o = put(o, "0x");
    for (int i = 28; i >= 0; i -= 4) *o++ = "0123456789ABCDEF"[(v >> i) & 15];
    return o;
}
static char *put_dec(char *o, uint32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *o++ = t[--n];
    return o;
}

static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

/* ---- files (core_load only) -------------------------------------------------------------------------------- */
static int f_open(struct fw_file *f, const char *path, int oflag)
{
    zero(f, sizeof *f);
    if (!FW_SD_MOUNTED()) return 0;                 /* SdFat stalls on an unmounted volume */
    return fw_file_open(f, fw_sd, path, oflag);
}
static int f_read(struct fw_file *f, void *buf, uint32_t n)
{
    if (f->fat) return fw_fat_read(f->fat, buf, n);
    if (f->exfat) return fw_exfat_read(f->exfat, buf, n);
    return -1;
}
static int f_write(struct fw_file *f, const void *buf, uint32_t n)
{
    if (f->fat) return fw_fat_write(f->fat, (void *)buf, n);
    if (f->exfat) return fw_exfat_write(f->exfat, (void *)buf, n);
    return -1;
}
static int read_exact(struct fw_file *f, void *buf, uint32_t n) { return f_read(f, buf, n) == (int)n; }
#ifndef CORE_SD_ONLY
static int f_seek(struct fw_file *f, uint32_t pos)
{
    if (f->fat) return fw_fat_seek(f->fat, pos);
    if (f->exfat) return fw_exfat_seek(f->exfat, 0, pos);
    return 0;
}
static int f_remove(const char *path)
{
    struct fw_file f;
    if (!f_open(&f, path, FW_O_RDWR)) return 0;
    int ok = f.fat ? fw_fat_remove(f.fat) : f.exfat ? fw_exfat_remove(f.exfat) : 0;
    fw_file_close(&f);
    return ok;
}

#endif

static int write_file(const char *path, const char *text, uint32_t n)
{
    struct fw_file f;
    if (!f_open(&f, path, FW_O_RDWR | FW_O_CREAT | FW_O_TRUNC)) return 0;
    int ok = f_write(&f, text, n) == (int)n;
    fw_file_close(&f);
    return ok;
}

#ifndef CORE_SD_ONLY
static void restart_now(void)
{
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u;  /* SCB_AIRCR: SYSRESETREQ */
    for (;;) { }
}

#endif

/* ---- the crash guard ------------------------------------------------------------------------------------------
 * /dirtyloader/guard holds SKIP once a boot with mods crashed; while it does, nothing is loaded. Anything else
 * (or no file) means load. Writing GOOD there (dirtyloader sd does) re-enables the mods. */
#define GUARD_PATH "/dirtyloader/guard"
#define G_SKIP 0x50494B53u   /* "SKIP" */
#define G_GOOD 0x444F4F47u
#define G_ARMD 0x444D5241u   /* obsolete startup marker from older builds */
enum { SKIP_NONE, SKIP_CRASH, SKIP_GUARD, SKIP_HOST = 4 };
enum { GUARD_PENDING, GUARD_CLEARED, GUARD_WRITE_ERROR };
static uint32_t read_guard(void)
{
    struct fw_file f;
    uint32_t w = 0;
    if (f_open(&f, GUARD_PATH, FW_O_READ)) {
        if (!read_exact(&f, &w, 4)) w = 0;
        fw_file_close(&f);
    }
    return w;
}

#if !defined(CORE_SD_ONLY) || defined(CORE_MODEL01_MENU)
static void guard_recover(void)
{
    if (ram->guard_recovery == GUARD_CLEARED) return;
    ram->guard_recovery =
        FW_SD_MOUNTED() && write_file(GUARD_PATH, "GOOD", 4) && read_guard() == G_GOOD
        ? GUARD_CLEARED : GUARD_WRITE_ERROR;
}
#endif


/* Teensyduino's fault handler leaves a record in RAM (len, ipsr, cfsr, hfsr, mmfar, bfar, pc, xpsr, ...) and reboots. */
static int crashed(void)     /* len is 11 (words) in this build, 44 (bytes) in some Teensyduino versions */
{
    return (fw_fault_info[0] == 11u || fw_fault_info[0] == 44u) && (fw_fault_info[2] | fw_fault_info[3]);
}
static void forget_crash(void) { ((volatile uint32_t *)fw_fault_info)[0] = 0; }

/* ---- loading ----------------------------------------------------------------------------------------------- */
/* .m8x header (docs/LOADER.md) */
struct m8x {
    uint32_t magic;              /* "DLX1", or "DLX2" */
    uint32_t image, bss, nrel;   /* bytes, bytes, relocation count */
    uint32_t crc;                /* crc32 of image then relocations */
    uint32_t data;               /* DLX2: where the writable part (.data) starts; 0 for DLX1 */
};
#define M8X_MAGIC  0x31584C44u
#define M8X_MAGIC2 0x32584C44u
#define M8X_HEAD1  20u
static int read_header(struct fw_file *f, struct m8x *h)
{
    if (!read_exact(f, h, M8X_HEAD1)) return 0;
    if (h->magic == M8X_MAGIC2) return read_exact(f, &h->data, 4) && h->data <= h->image && !(h->data & 3);
    h->data = 0;
    return h->magic == M8X_MAGIC;
}

enum { E_OPEN = 1, E_HEADER, E_SPACE, E_READ, E_CRC, E_MOD, E_FIRMWARE, E_DUPLICATE, E_TABLE, E_COUNT, E_FLASH };
static const char *const ERRORS[] = {
    "", "cannot open", "not an .m8x", "no room", "read failed", "checksum", "not a dl_mod",
    "built for other firmware", "id already loaded", "hook table full", "too many mods", "flash write failed",
};

static int add_hook(uint16_t event, uint16_t order, void *fn, uint16_t *orders)
{
    if (ram->count >= SLOTS) return 0;
    uint32_t i = ram->count;
    while (i && orders[i - 1] > order) {           /* stable: after hooks of the same order */
        ram->table[i] = ram->table[i - 1];
        orders[i] = orders[i - 1];
        i--;
    }
    ram->table[i].event = event;
    ram->table[i].flags = FLAG_ENABLED;
    ram->table[i].fn = fn;
    orders[i] = order;
    ram->count++;
    return 1;
}

static void flash_hooks(uint16_t *orders)
{
    const struct hook *flash = (const struct hook *)CORE_FLASH_TABLE;
    ram->count = 0;
    for (uint32_t i = 0; i < CORE_FLASH_SLOTS; i++)       /* the flash-linked hooks keep their order */
        if (flash[i].fn && (flash[i].flags & FLAG_ENABLED))
            add_hook(flash[i].event, 0, flash[i].fn, orders);
}

#ifdef FW_MOD_FLASH
#define SCB_ICIALLU (*(volatile uint32_t *)0xE000EF50u)
/* Make flash at `at` (4 KB-aligned) hold n bytes of code: 0 it already did, 1 written, -1 failed. The firmware's
   routines run from ITCM with interrupts off and drop the D-cache lines they touch; the I-cache is ours to drop. */
static int flash_install(uint32_t at, const uint8_t *code, uint32_t n)
{
    const volatile uint8_t *fl = (const volatile uint8_t *)at;
    uint32_t i = 0;
    while (i < n && fl[i] == code[i]) i++;
    if (i == n) return 0;
    for (uint32_t a = at; a < at + n; a += 0x1000u) fw_flash_erase_sector((void *)a);
    for (uint32_t o = 0; o < n; o += 256u) fw_flash_write((void *)(at + o), code + o, n - o < 256u ? n - o : 256u);
    __asm volatile("dsb\n isb" ::: "memory");
    SCB_ICIALLU = 0;
    __asm volatile("dsb\n isb" ::: "memory");
    for (i = 0; i < n; i++)
        if (fl[i] != code[i]) return -1;
    return 1;
}
#endif

static int load_one(const char *name, uint16_t *orders, const struct dl_mod **out)
{
    char path[96];
    char *o = put(path, "/dirtyloader/mods/");
    for (uint32_t i = 0; name[i] && o < path + sizeof path - 1; i++) *o++ = name[i];
    *o = 0;

    struct fw_file f;
    struct m8x h;
    if (!f_open(&f, path, FW_O_READ)) return E_OPEN;
    int err = 0;
    uint32_t base = ram->next, flash_was = ram->flash_next;
    const struct dl_mod *m = 0;
    ram->last_flash = ram->last_flash_n = ram->last_written = 0;
    if (!read_header(&f, &h) || h.image < sizeof(struct dl_mod)
        || h.image > 0x100000u || h.nrel > 0x40000u || h.bss > AREA_END) {
        err = E_HEADER;
        goto done;
    }
    uint32_t tail = h.nrel * 4 > h.bss ? h.nrel * 4 : h.bss;
    if (base + h.image + tail <= AREA_END) {
        /* ITCM: the whole image, relocated where it runs */
        uint8_t *img = (uint8_t *)base;
        uint32_t *rel = (uint32_t *)(base + h.image);   /* relocations sit where .bss will be */
        if (!read_exact(&f, img, h.image) || !read_exact(&f, rel, h.nrel * 4)) { err = E_READ; goto done; }
        if (crc32(crc32(0, img, h.image), (uint8_t *)rel, h.nrel * 4) != h.crc) { err = E_CRC; goto done; }
        for (uint32_t i = 0; i < h.nrel; i++)
            if (rel[i] > h.image - 4 || (rel[i] & 3)) { err = E_HEADER; goto done; }
        for (uint32_t i = 0; i < h.nrel; i++)
            *(uint32_t *)(img + rel[i]) += base;
        zero(rel, tail);
        m = (const struct dl_mod *)img;
        if (m->magic != DL_MOD_MAGIC || m->abi != DL_ABI) { err = E_MOD; goto done; }
        if (m->fw && m->fw != DL_FW) { err = E_FIRMWARE; goto done; }
        /* the relocations are spent: the mod keeps its image and .bss, and the next one may load over the rest (on
           a Model:02, CHAOS CONTROL's 342 relocations held 1.1 KB of ITCM past its 212 B of .bss) */
        ram->next = (base + h.image + h.bss + 7u) & ~7u;
    }
#ifdef FW_MOD_FLASH
    else if (h.data >= sizeof(struct dl_mod)) {
        /* flash (core 0.7, DLX2): code and constants (image[0, data)) run from FW_MOD_FLASH, .data and .bss sit in
           ITCM. The code is relocated in the spare DTCM (nothing has claimed it before the mods' ev_boot) and
           written only if flash doesn't already hold exactly that: an ordinary boot writes nothing. */
        uint32_t code = h.data, wdata = h.image - h.data;
        uint32_t at = ram->flash_next;
        uint8_t *stage = (uint8_t *)FW_SPARE_RAM, *rw = (uint8_t *)base;
        uint32_t *rel = (uint32_t *)(FW_SPARE_RAM + ((code + 3u) & ~3u));
        if (base + wdata + h.bss > AREA_END || at + code > FW_MOD_FLASH_END
            || (uint32_t)rel + h.nrel * 4 > FW_SPARE_RAM_END) { err = E_SPACE; goto done; }
        if (!read_exact(&f, stage, code) || !read_exact(&f, rw, wdata) || !read_exact(&f, rel, h.nrel * 4)) {
            err = E_READ;
            goto done;
        }
        if (crc32(crc32(crc32(0, stage, code), rw, wdata), (uint8_t *)rel, h.nrel * 4) != h.crc) { err = E_CRC; goto done; }
        for (uint32_t i = 0; i < h.nrel; i++) {
            uint32_t off = rel[i];
            if (off > h.image - 4 || (off & 3)) { err = E_HEADER; goto done; }
            uint32_t *w = (uint32_t *)(off < code ? stage + off : rw + (off - code));
            if (*w > h.image + h.bss) { err = E_HEADER; goto done; }
            *w = *w < code ? at + *w : base + (*w - code);
        }
        zero(rw + wdata, h.bss);
        const struct dl_mod *sm = (const struct dl_mod *)stage;
        if (sm->magic != DL_MOD_MAGIC || sm->abi != DL_ABI) { err = E_MOD; goto done; }
        if (sm->fw && sm->fw != DL_FW) { err = E_FIRMWARE; goto done; }
        int w = flash_install(at, stage, code);
        if (w < 0) { err = E_FLASH; goto done; }
        m = (const struct dl_mod *)at;
        ram->next = (base + wdata + h.bss + 7u) & ~7u;
        ram->flash_next = (at + code + 0xFFFu) & ~0xFFFu;
        ram->last_flash = at;
        ram->last_flash_n = code;
        ram->last_written = (uint32_t)w;
    }
#endif
    else {
        err = E_SPACE;
        goto done;
    }
    for (uint32_t i = 0; i < ram->nmods; i++)
        if (seq(ram->mods[i]->id, m->id)) { err = E_DUPLICATE; goto done; }
    if (ram->nmods >= MAX_MODS) { err = E_COUNT; goto done; }
    if (ram->count + m->nsubs > SLOTS) { err = E_TABLE; goto done; }
    for (uint32_t i = 0; i < m->nsubs; i++)         /* DL_INFO's entry is for the MOD LOADER, not a hook */
        if (m->subs[i].event != EV_INFO) add_hook(m->subs[i].event, m->subs[i].order, m->subs[i].fn, orders);
    ram->mod_lo[ram->nmods] = ram->last_flash ? ram->last_flash : base;
    ram->mod_hi[ram->nmods] = ram->last_flash ? ram->last_flash + ram->last_flash_n : base + h.image;
    ram->mods[ram->nmods++] = m;
    *out = m;
done:
    if (err) { ram->next = base; ram->flash_next = flash_was; ram->last_flash = 0; }   /* a refused mod leaves its room */
    fw_file_close(&f);
    return err;
}



/* Dispatch a three-argument event from a firmware wrapper. */
#ifndef CORE_DISPATCH
#error "CORE_DISPATCH comes from the builder"
#endif
#define TAKE 0x10000u
static int fire3(uint32_t word, uint32_t a0, uint32_t a1, uint32_t a2)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t ip __asm__("ip") = word;
    register uint32_t fn __asm__("r3") = CORE_DISPATCH;
    __asm volatile("blx r3" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(ip), "+r"(fn) : : "lr", "memory", "cc");
    return (int)r0;
}

static uint32_t has_subs(uint16_t ev)
{
    for (uint32_t i = 0; i < ram->count; i++) if (ram->table[i].event == ev) return 1;
    return 0;
}
static void wrap_slot(uint32_t slot, uint32_t *orig, uint32_t fn)
{
    volatile uint32_t *s = (volatile uint32_t *)slot;
    if (*s != (fn | 1)) { *orig = *s; *s = fn | 1; }
}

/* ev_audio_out: the tap's update() (vtable slot 0, in RAM) gets the master output's two blocks first: a handler
   processes them in place (insert: USB, the recorder and I2S all get it) or writes them (exclusive). A silent
   output has no blocks: with handlers, core allocates zeroed ones so they can write. */
#define EV_AUDIO_OUT_W (0x0032u | TAKE)
static void core_tap(void *obj)
{
    uint8_t **q = *(uint8_t ***)((uint8_t *)obj + 0x20);
    if (ram->audio_subs && q) {
        for (int i = 0; i < 2; i++)
            if (!q[i] && (q[i] = fw_audio_alloc()) != 0) zero(q[i] + 4, FW_AUDIO_FRAMES * 4);
        if (q[0] && q[1]) fire3(EV_AUDIO_OUT_W, (uint32_t)(q[0] + 4), (uint32_t)(q[1] + 4), FW_AUDIO_FRAMES);
    }
    ((void (*)(void *))ram->tap_orig)(obj);
}

/* The track's main strip and sends share its rendered audio blocks. */
#define EV_TRACK_AUDIO_W 0x0033u
typedef void (*update_fn)(void *);
static void core_track(void *obj)
{
    ((update_fn)ram->track_orig)(obj);
    if (!ram->track_subs) return;
    uint8_t *strip = FW_TRACK_STRIP(obj);
    uint8_t **q = *(uint8_t ***)(strip + 0x20);
    uint32_t inputs = strip[0x18];                  /* the track sends on ports 0/1, and 2/3 while it crossfades */
    for (uint32_t p = 0; q && p + 1 < inputs && p < 4; p += 2)
        if (q[p] && q[p + 1])
            fire3(EV_TRACK_AUDIO_W, (uint32_t)(q[p] + 4), (uint32_t)(q[p + 1] + 4), ((uint8_t *)obj)[0x3D]);
}

#ifdef CORE_SD_ONLY
void core_tick(void) { }
#ifdef CORE_MODEL01_MENU
#include "model01_menu.h"
#endif
static int api_file_write(const char *path, const void *buf, uint32_t n);

static const struct dl_core api = {
    .magic = DL_CORE_SD_MAGIC,
    .version = DL_CORE_VERSION,
    .size = sizeof(struct dl_core),
    .file_write = api_file_write,
};
#define BOOT_API (&api)
#else
/* ---- entry points ------------------------------------------------------------------------------------------ */
/* ---- mods over USB (docs/LOADER.md, "Transfer") ------------------------------------------------------------
 * Requests travel in the m8c serial stream: 0xD1, cmd, seq, len (u16 LE), payload[len], sum8 (cmd..payload).
 * m8c's own commands are ASCII, so 0xD1 never starts one. core_serial, subscribed to ev_serial at order 0,
 * sees each request before the m8c parser can drain it. Replies are SLIP frames like the display stream's:
 * 0xF1, cmd, seq, status, data. Files live under /dirtyloader; nothing here loads a mod: the next boot does. */
#define PKT_MARK  0xD1
#define PKT_REPLY 0xF1
#define PKT_MAX   1100u                             /* payload bytes; the request lands in SCRATCH */
enum { C_INFO = 1, C_LIST, C_PUT, C_CHECK, C_GET, C_MKDIRS, C_RESTART, C_PEEK, C_DELETE, C_POKE, C_THEME, C_THEMEX };
enum { S_OK = 0, S_BAD, S_SUM, S_PATH, S_IO, S_NOCARD, S_TIMEOUT, S_CMD };

static int rx_peek(void)
{
    int c = -1;
    FW_NVIC_ICER3 = FW_USB_IRQ_BIT;
    uint8_t t = fw_rx_tail;
    if (fw_rx_head != t) {
        if (++t >= FW_RX_RING) t = 0;
        uint8_t i = fw_rx_list[t];
        c = fw_rx_buffer[i * FW_RX_BUF + fw_rx_index[i]];
    }
    FW_NVIC_ISER3 = FW_USB_IRQ_BIT;
    return c;
}

static int rx_exact(uint8_t *p, uint32_t n, uint32_t ms)
{
    uint32_t t0 = fw_millis;
    while (n) {
        uint32_t k = fw_rx_available;
        if (k) {
            if (k > n) k = n;
            int got = fw_serial_read(p, k);
            if (got > 0) { p += got; n -= (uint32_t)got; t0 = fw_millis; }
        } else if (fw_millis - t0 > ms) {
            return 0;
        }
    }
    return 1;
}

static void tx(uint8_t c)
{
    if (c == 0xC0) { fw_serial_putc(0xDB); fw_serial_putc(0xDC); }
    else if (c == 0xDB) { fw_serial_putc(0xDB); fw_serial_putc(0xDD); }
    else fw_serial_putc(c);
}
static void reply(uint8_t cmd, uint8_t seq, uint8_t status, const void *data, uint32_t n)
{
    const uint8_t *d = data;
    tx(PKT_REPLY); tx(cmd); tx(seq); tx(status);
    while (n--) tx(*d++);
    fw_serial_putc(0xC0);
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* "/dirtyloader/" + a relative name of [A-Za-z0-9._-/], no "..", under 64 bytes */
/* "/dirtyloader/" + a relative name; with `anywhere` (reads only) an absolute path is taken as it is */
static int dl_path_in(char *out, const uint8_t *name, uint32_t n, int anywhere)
{
    if (!n || n > 63 || (name[0] == '/' && !anywhere)) return 0;
    char *o = name[0] == '/' ? out : put(out, "/dirtyloader/");
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_'
                 || c == '-' || c == '/';
        if (!ok || (c == '.' && i + 1 < n && name[i + 1] == '.')) return 0;
        *o++ = (char)c;
    }
    *o = 0;
    return 1;
}
static int dl_path(char *out, const uint8_t *name, uint32_t n) { return dl_path_in(out, name, n, 0); }
static int rd_path(char *out, const uint8_t *name, uint32_t n) { return dl_path_in(out, name, n, 1); }
#ifdef PUT_ANYWHERE
#define PUT_PATH rd_path
#else
#define PUT_PATH dl_path
#endif


static int mkdirs(const char *path)
{
    uint32_t t[0x40 / 4];
    zero(t, sizeof t);
    if (fw_fat_vol) {
        if (fw_fat_mkdir(t, (uint8_t *)fw_fat_vol + FW_VOL_CWD, path, 1)) fw_fat_fclose(t);
    } else if (fw_exfat_vol) {
        if (fw_exfat_mkdir(t, (uint8_t *)fw_exfat_vol + FW_VOL_CWD, path, 1)) fw_exfat_fclose(t);
    }
    struct fw_file f;
    int ok = f_open(&f, path, FW_O_READ);
    if (ok) fw_file_close(&f);
    return ok;
}

#ifdef fw_sys
/* The theme, as the shop sets it: the 13 colours (39 bytes, the .m8t order) into the system settings, the
 * palette set from them, and every view repainted -- what the THEME page's LOAD does, without a file. */
static void theme_apply(const uint8_t *c)
{
    for (int i = 0; i < 39; i++) fw_sys[FW_SYS_THEME + i] = c[i];
    for (int i = 0; i < 13; i++) fw_set_pal(0, i, c[i * 3], c[i * 3 + 1], c[i * 3 + 2]);
    fw_redraw(FW_REDRAW_OBJ, 1);
}
#endif

#if defined(FW_ATTR_PAL_LIT) && defined(fw_sys)
/* ---- the extended theme: 15 colours past the firmware's 13 -------------------------------------------------------
 * The draw loop turns each cell's attribute byte into a palette index, and 'D'..'R' reach 13..27, which no screen
 * uses. Core gives the loop a 28-entry palette (FW_ATTR_PAL_LIT): the firmware's 13, copied before each refresh, then
 * these. Before each refresh that will draw, it rewrites the attribute bytes of the plain text it recolours:
 *   0-7   TRACK 1..8: SONG's track columns (the header digit and the chains under it) and the track list ('1'..'8'
 *         down one column, on every screen that shows it)
 *   8-14  the title (TEXT:TITLES) per screen: SONG, CHAIN, PHRASE, INSTRUMENT (+ MODS, POOL), TABLE, MIXER (+ EFFECT
 *         SETTINGS, EQ, LIMITER), everything else
 *   XT_FOLLOW: CHAIN, PHRASE, TABLE and INSTRUMENT titles take the current track's colour (the track list's cursor)
 * The cursor, selection and play markers are never touched. /dirtyloader/theme.ext keeps it: "DLXT", flags, 3 zero
 * bytes, the 15 colours (R G B). */
#define XT_N 15
#define XT_PATH "/dirtyloader/theme.ext"
#define XT_FILE 53u
enum { XT_TRACKS = 1, XT_TITLES = 2, XT_FOLLOW = 4 };
static uint32_t rgb565(const uint8_t *c) { return (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 2) << 5 | c[2] >> 3; }
static void xt_colours(void) { for (int k = 0; k < XT_N; k++) ram->xt_pal[13 + k] = rgb565(ram->xt_rgb + 3 * k); }
static int xt_idx(uint8_t c)                    /* the draw loop's decoding; -1: skipped */
{
    uint8_t r = (uint8_t)(c - 0x2Eu);
    return r <= 9 ? r : r <= 0x24 ? c - 0x37 : -1;
}
static int xt_plain(uint8_t c, int lo, int hi)  /* text of colour lo..hi, not a cursor or selection block */
{
    int i = xt_idx(c);
    return c != '5' && c != '6' && i >= lo && i <= hi;
}
static int xt_alnum(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); }
static void xt_paint(void)
{
    uint8_t *ch = fw_grid + FW_GRID_CHARS, *at = fw_grid + FW_GRID_ATTR;
    const int W = FW_GRID_COLS, H = FW_GRID_ROWS;
    const uint8_t f = ram->xt_flags;
    int cur = -1, found = 0;
    /* the track list: '1'..'8' down one column in the right half, each alone in its cell; not CHAIN's or PHRASE's
       row numbers (a '0' above them, a '9' below) */
    for (int c = W / 2; c < W - 1 && !found; c++)
        for (int r = 1; r + 9 <= H; r++) {
            int k = 0;
            while (k < 8 && ch[(r + k) * W + c] == '1' + k && !xt_alnum(ch[(r + k) * W + c - 1])
                   && !xt_alnum(ch[(r + k) * W + c + 1])) k++;
            if (k < 8 || ch[(r - 1) * W + c] == '0' || ch[(r + 8) * W + c] == '9') continue;
            for (int t = 0; t < 8; t++) {
                uint8_t *a = &at[(r + t) * W + c];
                if (xt_idx(*a) == 7) cur = t;
                else if ((f & XT_TRACKS) && xt_plain(*a, 2, 4)) *a = (uint8_t)('D' + t);
            }
            found = 1;
            break;
        }
    uint32_t view = fw_ui[FW_UI_VIEW];
    if ((f & XT_TRACKS) && view == 0) {         /* SONG: the header's '1'..'8' (3 apart) and the chains under each */
        int done = 0;
        for (int r = 0; r < 10 && !done; r++)
            for (int c = 0; c + 22 <= W; c++) {
                int k = 0;
                while (k < 8 && ch[r * W + c + 3 * k] == '1' + k) k++;
                if (k < 8) continue;
                for (int t = 0; t < 8; t++) {
                    uint8_t *a = &at[r * W + c + 3 * t];
                    if (xt_plain(*a, 2, 4)) *a = (uint8_t)('D' + t);
                    for (int rr = r + 1; rr < H; rr++)
                        for (int d = 0; d < 2; d++) {
                            a = &at[rr * W + c + 3 * t + d];
                            if (xt_plain(*a, 4, 4)) *a = (uint8_t)('D' + t);
                        }
                }
                done = 1;
                break;
            }
    }
    if (f & (XT_TITLES | XT_FOLLOW)) {
        int slot = -1, per_track = view == 2 || view == 3 || view == 4 || view == 5 || view == 0x16;
        if ((f & XT_FOLLOW) && per_track && cur >= 0) slot = cur;
        else if (f & XT_TITLES)
            slot = 8 + (view == 0 ? 0 : view == 2 ? 1 : view == 3 ? 2 : view == 5 || view == 0x16 || view == 0x18 ? 3
                        : view == 4 ? 4 : view == 0x0D || view == 0x0B || view == 0x19 || view == 0x1A ? 5 : 6);
        if (slot >= 0)
            for (int i = 0; i < W * H; i++) if (xt_plain(at[i], 5, 5)) at[i] = (uint8_t)('D' + slot);
    }
}
/* ev_draw, last: before the frame refresh, after the screen has written its text */
static void xt_draw(void *arg)
{
    (void)arg;
    for (int i = 0; i < 13; i++) ram->xt_pal[i] = FW_PALETTE[i];
    if (!ram->xt_flags || ram->page_mode || fw_millis - fw_frame_last < FW_FRAME_MS) return;   /* this pass won't draw */
    xt_paint();
}
static void xt_boot(void)
{
    ram->xt_on = 0;
    ram->xt_flags = 0;
    zero(ram->xt_rgb, sizeof ram->xt_rgb);
    struct fw_file f;
    uint8_t b[XT_FILE];
    if (FW_SD_MOUNTED() && f_open(&f, XT_PATH, FW_O_READ)) {
        int n = f_read(&f, b, XT_FILE);
        fw_file_close(&f);
        if (n == (int)XT_FILE && b[0] == 'D' && b[1] == 'L' && b[2] == 'X' && b[3] == 'T') {
            ram->xt_flags = b[4] & 7u;
            for (uint32_t i = 0; i < sizeof ram->xt_rgb; i++) ram->xt_rgb[i] = b[8 + i];
        }
    }
    for (int i = 0; i < 13; i++) ram->xt_pal[i] = FW_PALETTE[i];
    xt_colours();
    if (*FW_ATTR_PAL_LIT != (uint32_t)FW_PALETTE || ram->count >= SLOTS) return;   /* not this firmware's loop */
    ram->table[ram->count].event = 0x0020u;      /* ev_draw, after every mod's handler */
    ram->table[ram->count].flags = FLAG_ENABLED;
    ram->table[ram->count].fn = (void *)xt_draw;
    ram->count++;
    __asm volatile("dsb" ::: "memory");
    *FW_ATTR_PAL_LIT = (uint32_t)ram->xt_pal;
    __asm volatile("dsb\n isb" ::: "memory");
    ram->xt_on = 1;
}
#endif

static void serve(uint8_t cmd, uint8_t seq, const uint8_t *p, uint32_t len)
{
    char path[96];
    uint8_t out[600];
    struct fw_file f;

    if (cmd == C_RESTART) {
        reply(cmd, seq, S_OK, 0, 0);
        uint32_t t = fw_millis;
        while (fw_millis - t < 50) { }                  /* let the reply leave */
        restart_now();
    }
    if (cmd == C_INFO) {
        char *o = (char *)out;
        o = put(o, "dirtyloader core 0.7\nfirmware ");
        o = put_hex(o, DL_FW);
        o = put(o, "\nsd ");
        o = put(o, FW_SD_MOUNTED() ? "mounted" : "none");
        o = put(o, "\nmods area ");
        o = put_dec(o, AREA_END - AREA_START);
        o = put(o, "\nloaded");
        for (uint32_t i = 0; i < ram->nmods && i < MAX_MODS; i++) { *o++ = ' '; o = put(o, ram->mods[i]->id); }
        o = put(o, "\n");
        reply(cmd, seq, S_OK, out, (uint32_t)(o - (char *)out));
        return;
    }
    if (cmd == C_PEEK) {                                /* address u32, length u16 (<= 512): read memory */
        if (len != 6) { reply(cmd, seq, S_BAD, 0, 0); return; }
        uint32_t at = le32(p), n = p[4] | p[5] << 8;
        if (n > 512) n = 512;
        reply(cmd, seq, S_OK, (const void *)at, n);
        return;
    }
    if (cmd == C_POKE) {                                /* address u32, bytes: write RAM (development, tests) */
        uint32_t at = len >= 4 ? le32(p) : 0, n = len - 4;
        if (len < 5 || at < 0x20000000u || at + n > 0x20280000u) { reply(cmd, seq, S_BAD, 0, 0); return; }
        for (uint32_t i = 0; i < n; i++) ((volatile uint8_t *)at)[i] = p[4 + i];
        reply(cmd, seq, S_OK, 0, 0);
        return;
    }
#ifdef fw_sys
    /* THEME: no payload reads the 13 colours and the name (39 + 13 bytes). 39 colour bytes apply them now; a 40th,
     * flags, bit 0: also save the settings to EEPROM (kept across power-off), so a preview while a colour is dragged
     * writes nothing; then up to 12 name characters (A-Z 0-9 space - _), shown on the THEME page. */
    if (cmd == C_THEME) {
        if (!len) {
            for (int i = 0; i < 39; i++) out[i] = fw_sys[FW_SYS_THEME + i];
            for (int i = 0; i < 13; i++) out[39 + i] = fw_sys[FW_SYS_THEME_NAME + i];
            reply(cmd, seq, S_OK, out, 52);
            return;
        }
        if (len < 39 || len > 52) { reply(cmd, seq, S_BAD, 0, 0); return; }
        theme_apply(p);
        if (len > 40) {
            for (uint32_t i = 0; i < 13; i++) {
                uint8_t c = i < len - 40 ? p[40 + i] : 0;
                if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
                if (c && !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')) c = ' ';
                fw_sys[FW_SYS_THEME_NAME + i] = i < 12 ? c : 0;
            }
        }
        if (len > 39 && (p[39] & 1)) fw_sys_save(fw_sys);
        reply(cmd, seq, S_OK, 0, 0);
        return;
    }
#endif
#if defined(FW_ATTR_PAL_LIT) && defined(fw_sys)
    /* THEMEX: the extended theme. No payload: flags (bit 0 track colours, 1 screen titles, 2 titles follow the
     * track) and the 15 colours (1 + 45 bytes). Flags and colours (46 bytes) set them and repaint every view; a 47th
     * byte, bit 0, also writes /dirtyloader/theme.ext so the next boot has them. */
    if (cmd == C_THEMEX) {
        if (!ram->xt_on) { reply(cmd, seq, S_CMD, 0, 0); return; }
        if (!len) {
            out[0] = ram->xt_flags;
            for (uint32_t i = 0; i < sizeof ram->xt_rgb; i++) out[1 + i] = ram->xt_rgb[i];
            reply(cmd, seq, S_OK, out, 46);
            return;
        }
        if (len != 46 && len != 47) { reply(cmd, seq, S_BAD, 0, 0); return; }
        ram->xt_flags = p[0] & 7u;
        for (uint32_t i = 0; i < sizeof ram->xt_rgb; i++) ram->xt_rgb[i] = p[1 + i];
        xt_colours();
        fw_redraw(FW_REDRAW_OBJ, 1);              /* every view writes its text and attributes again */
        if (len == 47 && (p[46] & 1)) {
            uint8_t *b = out;
            b[0] = 'D'; b[1] = 'L'; b[2] = 'X'; b[3] = 'T'; b[4] = ram->xt_flags; b[5] = b[6] = b[7] = 0;
            for (uint32_t i = 0; i < sizeof ram->xt_rgb; i++) b[8 + i] = ram->xt_rgb[i];
            if (!FW_SD_MOUNTED() || !write_file(XT_PATH, (const char *)b, XT_FILE)) { reply(cmd, seq, S_IO, 0, 0); return; }
        }
        reply(cmd, seq, S_OK, 0, 0);
        return;
    }
#endif
    if (!FW_SD_MOUNTED()) { reply(cmd, seq, S_NOCARD, 0, 0); return; }
    if (cmd == C_MKDIRS) {
        reply(cmd, seq, mkdirs("/dirtyloader/mods") ? S_OK : S_IO, 0, 0);
        return;
    }
    if (cmd == C_LIST) {                                /* names in /dirtyloader/<payload>, one per line */
        struct fw_file dir;
        if (!rd_path(path, p, len) || !f_open(&dir, path, FW_O_READ)) { reply(cmd, seq, S_PATH, 0, 0); return; }
        uint32_t n = 0;
        for (;;) {
            zero(&f, sizeof f);
            if (!fw_file_open_next(&f, &dir, FW_O_READ)) break;
            char name[64];
            int k = f.fat ? fw_fat_name(f.fat, name, sizeof name) : fw_exfat_name(f.exfat, name, sizeof name);
            fw_file_close(&f);
            if (k <= 0 || n + (uint32_t)k + 1 > sizeof out) continue;
            for (int i = 0; i < k; i++) out[n++] = (uint8_t)name[i];
            out[n++] = '\n';
        }
        fw_file_close(&dir);
        reply(cmd, seq, S_OK, out, n);
        return;
    }
    if (cmd == C_PUT) {                                 /* offset u32, name len u8, name, data */
        if (len < 5 || len < 5u + p[4] || !PUT_PATH(path, p + 5, p[4])) { reply(cmd, seq, S_PATH, 0, 0); return; }
        uint32_t off = le32(p), skip = 5u + p[4];
        int oflag = off ? FW_O_RDWR : (FW_O_RDWR | FW_O_CREAT | FW_O_TRUNC);
        if (!f_open(&f, path, oflag)) { reply(cmd, seq, S_IO, 0, 0); return; }
        int w = (off && !f_seek(&f, off)) ? -1 : f_write(&f, p + skip, len - skip);
        fw_file_close(&f);
        put32(out, w < 0 ? 0 : (uint32_t)w);
        reply(cmd, seq, w == (int)(len - skip) ? S_OK : S_IO, out, 4);
        return;
    }
    if (cmd == C_CHECK) {                               /* name -> size u32, crc32 u32 */
        if (!rd_path(path, p, len) || !f_open(&f, path, FW_O_READ)) { reply(cmd, seq, S_PATH, 0, 0); return; }
        uint32_t size = 0, crc = 0;
        int k;
        while ((k = f_read(&f, out, 512)) > 0) { crc = crc32(crc, out, (uint32_t)k); size += (uint32_t)k; }
        fw_file_close(&f);
        put32(out, size);
        put32(out + 4, crc);
        reply(cmd, seq, k < 0 ? S_IO : S_OK, out, 8);
        return;
    }
    if (cmd == C_DELETE) {                              /* name: a file under /dirtyloader */
        if (!dl_path(path, p, len)) { reply(cmd, seq, S_PATH, 0, 0); return; }
        reply(cmd, seq, f_remove(path) ? S_OK : S_PATH, 0, 0);
        return;
    }
    if (cmd == C_GET) {                                 /* offset u32, max u16, name len u8, name -> data */
        if (len < 7 || len < 7u + p[6] || !rd_path(path, p + 7, p[6])) { reply(cmd, seq, S_PATH, 0, 0); return; }
        uint32_t off = le32(p), max = p[4] | p[5] << 8;
        if (max > 512) max = 512;
        if (!f_open(&f, path, FW_O_READ)) { reply(cmd, seq, S_PATH, 0, 0); return; }
        int k = (off && !f_seek(&f, off)) ? -1 : f_read(&f, out, max);
        fw_file_close(&f);
        reply(cmd, seq, k < 0 ? S_IO : S_OK, out, k < 0 ? 0 : (uint32_t)k);
        return;
    }
    reply(cmd, seq, S_CMD, 0, 0);
}

#ifndef FW_VECTORS
#error "fw.h names the RAM vector table (FW_VECTORS)"
#endif

int core_serial(void *m8c)
{
    if (rx_peek() != PKT_MARK) return 0;                /* not ours: the m8c parser runs */
    uint8_t h[5];
    if (!rx_exact(h, 5, 200)) return 1;
    uint32_t len = h[3] | h[4] << 8;
    uint8_t *buf = (uint8_t *)SCRATCH;
    if (len > PKT_MAX) { reply(h[1], h[2], S_BAD, 0, 0); return 1; }
    if (!rx_exact(buf, len + 1, 500)) { reply(h[1], h[2], S_TIMEOUT, 0, 0); return 1; }
    uint8_t sum = (uint8_t)(h[1] + h[2] + h[3] + h[4]);
    for (uint32_t i = 0; i < len; i++) sum += buf[i];
    if (sum != buf[len]) { reply(h[1], h[2], S_SUM, 0, 0); return 1; }
    serve(h[1], h[2], buf, len);
    return 1;
}

/* ---- the MODS page (TELOS) -----------------------------------------------------------------------------------
 * SYSTEM SETTINGS > MODS (TELOS) > OPEN. A screen of the firmware's own kind, in its free view slot: one row
 * per .m8x on the card with its state, APPLY & RESTART last. UP/DOWN move, EDIT switches a mod on or off for the
 * next boot (written to active.txt at once) or restarts on the last row, OPT goes back to SYSTEM SETTINGS.
 *   ON running   +ON on from the next boot   OFF   -OFF off from the next boot   E<n> on, but the boot refused it */
#ifndef SETTINGS_MODS_Y
#define SETTINGS_MODS_Y 12                          /* the field row core's settings template adds */
#endif
#define PAGE_TOP  5                                 /* grid row of the first mod */

static int name_eq(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static void page_read(void)
{
    struct fw_file dir, f;
    ram->page_n = 0;
    if (!f_open(&dir, "/dirtyloader/mods", FW_O_READ)) return;
    while (ram->page_n < MAX_MODS) {
        zero(&f, sizeof f);
        if (!fw_file_open_next(&f, &dir, FW_O_READ)) break;
        char *nm = ram->page[ram->page_n].name;
        int k = f.fat ? fw_fat_name(f.fat, nm, NAME_MAX) : fw_exfat_name(f.exfat, nm, NAME_MAX);
        fw_file_close(&f);
        if (k > 4 && name_eq(nm + k - 4, ".m8x", 5)) { ram->page[ram->page_n].check = 0; ram->page[ram->page_n++].on = 0; }
    }
    fw_file_close(&dir);
    char *const list = (char *)(SCRATCH + STATUS_SIZE);
    int n = 0;
    if (f_open(&f, "/dirtyloader/active.txt", FW_O_READ)) {
        n = f_read(&f, list, LIST_SIZE - 1);
        fw_file_close(&f);
    }
    if (n < 0) n = 0;
    list[n] = 0;
    for (char *p = list; *p;) {
        char *line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = 0;
        for (uint32_t i = 0; i < ram->page_n; i++)
            if (name_eq(ram->page[i].name, line, NAME_MAX)) ram->page[i].on = 1;
    }
}

static void page_write(void)
{
    char *const t = (char *)(SCRATCH + STATUS_SIZE);
    char *o = put(t, "# one .m8x per line, from /dirtyloader/mods/; core loads them at boot\n");
    for (uint32_t i = 0; i < ram->page_n; i++)
        if (ram->page[i].on) { o = put(o, ram->page[i].name); *o++ = '\n'; }
    write_file("/dirtyloader/active.txt", t, (uint32_t)(o - t));
}

static int boot_err(const char *name)               /* -1 not loaded at boot, 0 running, n refused */
{
    for (uint32_t i = 0; i < ram->nboot; i++)
        if (name_eq(ram->boot[i].name, name, NAME_MAX)) return ram->boot[i].err;
    return -1;
}

static void row(int r, const char *s)               /* a whole grid row, padded */
{
    char line[FW_GRID_COLS + 1];
    uint32_t i = 0;
    for (; s[i] && i < FW_GRID_COLS; i++) line[i] = s[i];
    for (; i < FW_GRID_COLS; i++) line[i] = ' ';
    line[FW_GRID_COLS] = 0;
    fw_text(fw_grid, 0, r, line);
}

static int page_changed(void)
{
    for (uint32_t i = 0; i < ram->page_n; i++) {
        int e = boot_err(ram->page[i].name);
        if (ram->page[i].on != (e >= 0)) return 1;
    }
    return 0;
}

/* the running mod an .m8x file became, and the page it added */
static const struct dl_mod *mod_of_file(const char *name)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < ram->nboot && k < ram->nmods; i++) {
        if (ram->boot[i].err) continue;
        if (name_eq(ram->boot[i].name, name, NAME_MAX)) return ram->mods[k];
        k++;
    }
    return 0;
}
static const struct dl_page *page_of_file(const char *name)
{
    const struct dl_mod *m = mod_of_file(name);
    for (uint32_t i = 0; m && i < ram->npages; i++)
        if (ram->pages[i]->mod && seq(ram->pages[i]->mod, m->id)) return ram->pages[i];
    return 0;
}

static void page_draw_all(void)
{
    char b[64], *o;
    for (int r = 0; r < FW_GRID_ROWS; r++) row(r, "");
    row(3, ram->skipped ? " SAFE MODE - MODS NOT LOADED" : " MODS (TELOS)");
    if (ram->skipped)
        row(4, ram->guard_recovery == GUARD_CLEARED ? " REBOOT TO RE-ENABLE MODS"
             : ram->guard_recovery == GUARD_WRITE_ERROR ? " SD: SET DIRTYLOADER/GUARD TO GOOD"
             : " PRESS PLAY TO EXIT SAFE MODE");
    for (uint32_t i = 0; i <= ram->page_n; i++) {
        o = b;
        *o++ = ' ';
        *o++ = i == ram->page_sel ? '>' : ' ';
        if (i == ram->page_n) {
            o = put(o, ram->skipped ? (ram->guard_recovery == GUARD_CLEARED ? "REBOOT TO RE-ENABLE MODS" : "CLEAR SKIP")
                                    : page_changed() ? "APPLY & RESTART" : "RESTART");
            *o = 0;
            row(PAGE_TOP + (int)i + 1, b);
            o = put(b, " ");
            *o++ = ram->page_sel == ram->page_n + 1 ? '>' : ' ';
            o = put(o, "MOD MENU");
            *o = 0;
            row(PAGE_TOP + (int)i + 2, b);
            break;
        }
        const char *nm = ram->page[i].name;
        uint32_t k = 0;
        while (nm[k] && k < 26) *o++ = nm[k++];
        while (k++ < 27) *o++ = ' ';
        int e = boot_err(nm);
        if (ram->page[i].on && e > 0) { *o++ = 'E'; o = put_dec(o, (uint32_t)e); }
        else if (!ram->page[i].on && ram->page[i].check) { *o++ = 'E'; o = put_dec(o, ram->page[i].check); *o++ = '!'; }
        else if (ram->page[i].on) o = put(o, e == 0 ? "ON" : "+ON");
        else o = put(o, e == 0 ? "-OFF" : "OFF");
        if (page_of_file(nm)) o = put(o, " >");
        *o = 0;
        row(PAGE_TOP + (int)i, b);
    }
    if (!ram->page_n) row(PAGE_TOP, "   NO MODS IN /DIRTYLOADER/MODS");
    o = put(b, " ");
    o = put_dec(o, ram->next - AREA_START);
    o = put(o, " / ");
    o = put_dec(o, AREA_END - AREA_START);
    o = put(o, " BYTES IN USE");
    *o = 0;
    row(20, b);
    row(21, " EDIT ON/OFF  LEFT OFF  RIGHT ON");    /* the firmware draws its track list from column 34 */
    row(22, " RIGHT ON A > ROW: THE MOD'S PAGE");
    row(23, " OPT BACK");
}

static void page_enter(void *view, int arg)
{
    if (ram->page_mode >= 2) { ram->page_dirty = 1; return; }   /* a mod's page, the Mod Menu */
    if (FW_SD_MOUNTED()) page_read();
    else ram->page_n = 0;
    ram->page_sel = 0;
    ram->page_dirty = 1;
}

static int wanted(const char *name)
{
    for (uint32_t i = 0; i < ram->nwant; i++)
        if (name_eq(ram->want[i].name, name, NAME_MAX)) return 1;
    return 0;
}

/* LOAD SONG MODS?: what YES would do to the mods on the card, and what it cannot (not on the card) */
static void song_draw_all(void)
{
    char b[64], *o;
    for (int r = 0; r < FW_GRID_ROWS; r++) row(r, "");
    row(3, " LOAD SONG MODS?");
    int r = PAGE_TOP;
    for (uint32_t i = 0; i < ram->page_n && r < 19; i++) {
        const char *nm = ram->page[i].name;
        int run = boot_err(nm) == 0, want = wanted(nm);
        if (run == want) continue;
        o = put(b, want ? "  + " : "  - ");
        o = put(o, nm);
        *o = 0;
        row(r++, b);
    }
    for (uint32_t i = 0; i < ram->nwant && r < 19; i++) {
        int found = 0;
        for (uint32_t k = 0; k < ram->page_n; k++)
            if (name_eq(ram->page[k].name, ram->want[i].name, NAME_MAX)) found = 1;
        if (found) continue;
        o = put(b, "  ? ");
        o = put(o, ram->want[i].name);
        o = put(o, "  NOT ON THE CARD");
        *o = 0;
        row(r++, b);
    }
    row(20, " + ON  - OFF  ? MISSING");
    row(21, " EDIT YES (RESTART)   OPT NO");
}

static void api_page_open(const struct dl_page *p);
static void api_page_close(void);

/* The check before switching on (core 0.5; TELOS lists it as "not yet"): what core_load would refuse, found
   now. The file is read once: the header, the size against the mods area, the CRC of image and relocations, and
   the dl_mod header's magic, ABI and firmware. A refusal leaves the mod off and shows E<n>! on its row. */
static int check_m8x(const char *name)
{
    char path[96];
    char *o = put(path, "/dirtyloader/mods/");
    for (uint32_t i = 0; name[i] && o < path + sizeof path - 1; i++) *o++ = name[i];
    *o = 0;
    struct fw_file f;
    struct m8x h;
    if (!f_open(&f, path, FW_O_READ)) return E_OPEN;
    int err = 0;
    uint8_t *buf = (uint8_t *)SCRATCH;               /* status text area: free after boot */
    if (!read_header(&f, &h) || h.image < sizeof(struct dl_mod)) { err = E_HEADER; goto done; }
    uint32_t tail = h.nrel * 4 > h.bss ? h.nrel * 4 : h.bss;
    int fits = h.image + tail <= AREA_END - AREA_START;
#ifdef FW_MOD_FLASH
    fits = fits || (h.data >= sizeof(struct dl_mod) && h.image - h.data + h.bss <= AREA_END - AREA_START
                    && h.data <= FW_MOD_FLASH_END - FW_MOD_FLASH
                    && ((h.data + 3u) & ~3u) + h.nrel * 4 <= FW_SPARE_RAM_END - FW_SPARE_RAM);
#endif
    if (!fits) { err = E_SPACE; goto done; }
    uint32_t left = h.image + h.nrel * 4, crc = 0, first = 1;
    while (left) {
        uint32_t n = left < STATUS_SIZE ? left : STATUS_SIZE;
        if (!read_exact(&f, buf, n)) { err = E_READ; goto done; }
        if (first) {
            const struct dl_mod *m = (const struct dl_mod *)buf;
            if (m->magic != DL_MOD_MAGIC || m->abi != DL_ABI) { err = E_MOD; goto done; }
            if (m->fw && m->fw != DL_FW) { err = E_FIRMWARE; goto done; }
            first = 0;
        }
        crc = crc32(crc, buf, n);
        left -= n;
    }
    if (crc != h.crc) err = E_CRC;
done:
    fw_file_close(&f);
    return err;
}

static void page_set(uint32_t i, uint8_t on)
{
    if (on && FW_SD_MOUNTED() && (ram->page[i].check = (uint8_t)check_m8x(ram->page[i].name)) != 0) return;
    ram->page[i].on = on;
    page_write();
}

static void menu_show(uint32_t back);
static void menu_input(int action);
static void mp_input(int action);
static void mp_draw(void);
static void mp_show(void);
static void mp_close(void);
#ifdef fw_rect
static void lm_input(int action);
static void lm_draw(void);
static void lm_show(void);
static void lm_close(void);
static void lm_leave(void);
#endif

static void page_input(void *view, int action)
{
    ram->last_action = (uint32_t)action;
    if (ram->page_mode == 4) { mp_input(action); return; }
#ifdef fw_rect
    if (ram->page_mode == 5) { lm_input(action); return; }
#endif
    if (ram->page_mode == 3) { menu_input(action); return; }
    if (ram->page_mode == 2) {                      /* a mod's page */
        const struct dl_page *p = ram->cur_page;
        if (p && p->key && p->key(action)) return;
        if (action == FW_KEY_OPT) api_page_close();
        return;
    }
    if (ram->page_mode) {                           /* LOAD SONG MODS? */
        if (action == FW_KEY_EDIT) {
            for (uint32_t i = 0; i < ram->page_n; i++) ram->page[i].on = (uint8_t)wanted(ram->page[i].name);
            page_write();
            restart_now();
        }
        if (action == FW_KEY_OPT) { ram->page_mode = 0; fw_set_view(fw_ui, (int)ram->prev_view); }
        return;
    }
    if (ram->skipped && action == FW_KEY_PLAY) {
        guard_recover();
        ram->page_dirty = 1;
        return;
    }
    uint32_t rows = ram->page_n + 2;                 /* the mods, RESTART, MOD MENU */
    if (action == FW_KEY_EDIT && ram->page_sel == ram->page_n + 1) { menu_show(PAGE_BACK_MODS); return; }
    if (action == FW_KEY_UP && ram->page_sel) ram->page_sel--;
    else if (action == FW_KEY_DOWN && ram->page_sel + 1 < rows) ram->page_sel++;
    else if (action == FW_KEY_OPT) { fw_set_view(fw_ui, FW_VIEW_SETTINGS); return; }
    else if (action == FW_KEY_EDIT) {
        if (ram->page_sel == ram->page_n) {
            if (!ram->skipped) restart_now();
            guard_recover();
            ram->page_dirty = 1;
            return;
        }
        page_set(ram->page_sel, !ram->page[ram->page_sel].on);
    }
    else if ((action == FW_KEY_RIGHT || action == FW_KEY_LEFT) && ram->page_sel < ram->page_n) {
        const struct dl_page *pg = page_of_file(ram->page[ram->page_sel].name);
        if (action == FW_KEY_RIGHT && pg) { api_page_open(pg); return; }
        uint8_t on = action == FW_KEY_RIGHT;          /* as TELOS: RIGHT on, LEFT off */
        if (ram->page[ram->page_sel].on != on) page_set(ram->page_sel, on);
    }
    ram->page_dirty = 1;
}

static void page_update(void *view) { }

static void menu_draw_all(void);

static void page_draw(void *view)
{
    if (ram->page_mode == 4) { mp_draw(); return; }
#ifdef fw_rect
    if (ram->page_mode == 5) { lm_draw(); return; }
#endif
    if (ram->page_mode == 2) {                      /* a mod's page: cleared once, then the mod draws */
        const struct dl_page *p = ram->cur_page;
        if (ram->page_dirty) {
            ram->page_dirty = 0;
            for (int r = 0; r < FW_GRID_ROWS; r++) row(r, "");
            if (p && p->title) { char b[44], *o = put(b, " "); o = put(o, p->title); *o = 0; row(3, b); }
        }
        if (p && p->draw) p->draw();
        return;
    }
    if (!ram->page_dirty) return;
    ram->page_dirty = 0;
    if (ram->page_mode == 3) menu_draw_all();
    else if (ram->page_mode) song_draw_all();
    else page_draw_all();
}

#ifdef FW_SIDE_0
/* +0x18: does this screen keep the firmware's right-hand column (tempo, the tracks, the view map, the meters)? +0x1C:
   its oscilloscope along the top? SETTINGS' methods say yes; core's full-screen pages (4 OPTIONS, 5 MOD LOADER) say
   no to both, and core_side* hold off the rest */
static int page_has_side(void *view) { (void)view; return !(ram->page_mode == 4u || ram->page_mode == 5u); }
#define PAGE_FLAG_18 page_has_side
#define PAGE_FLAG_1C page_has_side
#else
#define PAGE_FLAG_18 FW_VIEW_FLAG_18
#define PAGE_FLAG_1C FW_VIEW_FLAG_1C
#endif
static const void *const page_vt[8] = {
    (const void *)page_enter, (const void *)FW_VIEW_SHARED_04, (const void *)page_input, (const void *)page_update,
    (const void *)page_draw, (const void *)FW_VIEW_NOP, (const void *)PAGE_FLAG_18, (const void *)PAGE_FLAG_1C,
};

/* Is core's screen reachable as view FW_VIEW_FREE? Headless: core put it in the firmware's views table. Model:02's
   set_view is a switch whose case for that id does nothing; core_view (ev_view, order 0) does the switch then. */
static int page_installed(void)
{
#if FW_HAS_VIEW_TABLE
    return fw_views[FW_VIEW_FREE] == &ram->page_view;
#else
    return ram->page_view.vt == page_vt;
#endif
}

#if !FW_HAS_VIEW_TABLE
/* set_view(ui, id) for core's id: leave the current screen (+0x14), make core's the current one (ui + 4, the id at
   +0x15, the previous at +0x17) and enter it, as the firmware's cases do; then take the call. */
int core_view(void *ui, int id)
{
    if (id != FW_VIEW_FREE || !page_installed()) return 0;
    uint8_t *u = (uint8_t *)ui;
    void *old = *(void **)(u + 4);
    if (old) ((void (*)(void *))(*(const void *const *const *)old)[5])(old);
    u[0x17] = u[FW_UI_VIEW];
    u[FW_UI_VIEW] = (uint8_t)id;
    *(void **)(u + 4) = &ram->page_view;
    page_enter(&ram->page_view, 0);
    return 1;
}
#endif

/* ev_key, order 0: EDIT on the settings row core adds opens the page */
int core_key(void *ui, int action)
{
#ifdef fw_rect
    /* the Model:02: all four directions open (and close) the MOD LOADER; its OPTIONS are the knobs. The firmware's
       visualizer, which the chord used to toggle, opens from the MOD LOADER (EDIT on CHAOS CONTROL); the chord
       still closes it. */
    if (action == 0xE4 && page_installed()) {
#ifdef FW_UI_VISUALIZER
        if (((uint8_t *)ui)[FW_UI_VISUALIZER]) return 0;
#endif
        int in_core = ((uint8_t *)ui)[FW_UI_VIEW] == FW_VIEW_FREE;
        if (in_core && ram->page_mode == 4) mp_close();
        else if (in_core && ram->page_mode == 5) lm_leave();
        else lm_show();
        return 1;
    }
#else
    if (action == 0x84 && page_installed()) {      /* headless: LEFT+RIGHT, the machine pages */
        int in_core = ((uint8_t *)ui)[FW_UI_VIEW] == FW_VIEW_FREE;
        if (in_core && ram->page_mode == 4) mp_close();
        else mp_show();
        return 1;
    }
#endif
    if (((uint8_t *)ui)[FW_UI_VIEW] == FW_VIEW_SETTINGS && fw_settings[5] == SETTINGS_MODS_Y
        && action == FW_KEY_EDIT && page_installed()) {
        ram->page_mode = 0;
        fw_set_view(ui, FW_VIEW_FREE);
        return 1;
    }
    return 0;
}

/* ---- songs and their mods ------------------------------------------------------------------------------------
 * After a save, <song>.mods lists the mods that were running (file, id, version per line). After a load (not
 * the boot's own), when the list differs from what runs, the MODS page asks LOAD SONG MODS?. A song saved
 * without a list says nothing. The SD work waits for core_tick: the firmware's own file is closed by then. */
/* what: 1 a load (the user's: data, then LOAD SONG MODS?), 2 a save, 3 the boot's load (data only) */
static void song_note(const char *path, uint32_t what)
{
    uint32_t i = 0;
    for (; path && path[i] && i < sizeof ram->song - 1; i++) ram->song[i] = path[i];
    ram->song[i] = 0;
    if (!i) return;
    ram->song_at = fw_millis;
    ram->song_tries = 0;
    ram->data_done = 0;
    ram->song_pending = what;
}

void core_song_load(void *songs, const char *path)
{
    song_note(path, ram->booted ? 1 : 3);           /* the AUTOSAVE load during setup(): its data, no prompt */
}

void core_song_save(void *songs, const char *path) { song_note(path, 2); }

static char *mods_path(char *out)                  /* put() does not terminate: these do */
{
    char *o = put(put(out, ram->song), ".mods");
    *o = 0;
    return o;
}

/* <song>.dldata: "DLD1", count u32, then per block: id[24], size u32, the bytes */
#define DATA_MAGIC 0x31444C44u
#define DATA_ID    24u
static char *data_path(char *out)
{
    char *o = put(put(out, ram->song), ".dldata");
    *o = 0;
    return o;
}

#if DEST_EXT
static void dests_prepare(void);
static void dest_load_done(void);
#endif
static int data_save(void)
{
    if (!ram->ndata) return 1;
#if DEST_EXT
    dests_prepare();
#endif
    char path[140];
    struct fw_file f;
    data_path(path);
    if (!f_open(&f, path, FW_O_RDWR | FW_O_CREAT | FW_O_TRUNC)) return 0;
    uint32_t h[2] = { DATA_MAGIC, ram->ndata };
    int ok = f_write(&f, h, 8) == 8;
    for (uint32_t i = 0; ok && i < ram->ndata; i++) {
        const struct dl_song_data *d = ram->data[i];
        char id[DATA_ID];
        zero(id, sizeof id);
        for (uint32_t k = 0; d->id[k] && k < DATA_ID - 1; k++) id[k] = d->id[k];
        ok = f_write(&f, id, DATA_ID) == DATA_ID && f_write(&f, &d->size, 4) == 4
             && f_write(&f, d->data, d->size) == (int)d->size;
    }
    fw_file_close(&f);
    return ok;
}

/* 0 done, 1 the card is still busy: try again */
static int data_load(void)
{
    if (!ram->ndata || ram->data_done) return 0;
    char path[140];
    struct fw_file f;
    uint32_t got[MAX_DATA];
    zero(got, sizeof got);
    data_path(path);
    if (f_open(&f, path, FW_O_READ)) {
        uint32_t h[2], pos = 8;
        if (read_exact(&f, h, 8) && h[0] == DATA_MAGIC) {
            for (uint32_t b = 0; b < h[1] && b < 64; b++) {
                char id[DATA_ID];
                uint32_t size;
                if (!read_exact(&f, id, DATA_ID) || !read_exact(&f, &size, 4)) break;
                pos += DATA_ID + 4;
                id[DATA_ID - 1] = 0;
                for (uint32_t i = 0; i < ram->ndata; i++) {
                    const struct dl_song_data *d = ram->data[i];
                    if (!name_eq(d->id, id, DATA_ID)) continue;
                    uint32_t n = size < d->size ? size : d->size;
                    if (read_exact(&f, d->data, n)) got[i] = n ? n : 1;
                }
                pos += size;
                if (!f_seek(&f, pos)) break;
            }
        }
        fw_file_close(&f);
    } else {
        if (!f_open(&f, ram->song, FW_O_READ)) return 1;  /* the card is busy: the song itself will not open */
        fw_file_close(&f);                                /* the song has no data */
    }
    ram->data_done = 1;
    for (uint32_t i = 0; i < ram->ndata; i++)
        if (ram->data[i]->loaded) ram->data[i]->loaded(got[i]);
    return 0;
}

static void song_saved(void)
{
    char path[140], *const t = (char *)(SCRATCH + STATUS_SIZE), *o = t;
    o = put(o, "# dirtyloader: the mods running when this song was saved (file id version)\n");
    uint32_t k = 0;
    for (uint32_t i = 0; i < ram->nboot && k < ram->nmods; i++) {
        if (ram->boot[i].err) continue;
        const struct dl_mod *m = ram->mods[k++];
        o = put(o, ram->boot[i].name); *o++ = ' ';
        o = put(o, m->id); *o++ = ' ';
        o = put(o, m->version); *o++ = '\n';
    }
    mods_path(path);
    ram->song_result = write_file(path, t, (uint32_t)(o - t)) && data_save() ? 1 : 2;
}

static int song_loaded(void)                       /* 0 done, 1 the list could not be read yet */
{
    char path[140];
    struct fw_file f;
    char *const t = (char *)(SCRATCH + STATUS_SIZE);
    mods_path(path);
    if (!f_open(&f, path, FW_O_READ)) return 1;     /* still busy, or saved without a list */
    int n = f_read(&f, t, LIST_SIZE - 1);
    fw_file_close(&f);
    if (n < 0) return 1;
    t[n] = 0;
    ram->nwant = 0;
    for (char *p = t; *p;) {
        char *line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = 0;
        if (*line == '#' || !*line || ram->nwant >= MAX_MODS) continue;
        uint32_t i = 0;
        while (line[i] && line[i] != ' ' && i < NAME_MAX - 1) { ram->want[ram->nwant].name[i] = line[i]; i++; }
        ram->want[ram->nwant++].name[i] = 0;
    }
    uint32_t same = 1, running = 0;
    for (uint32_t i = 0; i < ram->nboot; i++)
        if (!ram->boot[i].err) { running++; if (!wanted(ram->boot[i].name)) same = 0; }
    if (same && running == ram->nwant) return 0;
    page_read();
    ram->page_mode = 1;
    ram->prev_view = fw_ui[FW_UI_VIEW];
    fw_set_view(fw_ui, FW_VIEW_FREE);
    return 0;
}

/* The firmware is still writing (or reading) the song for a while after its routine is entered, and a file
   core creates meanwhile fails: wait SONG_WAIT ms, and retry a failed write, before giving up. */
#define SONG_WAIT  300u
#define SONG_TRIES 10u
#define EV_USB_W 0x0003u
static int fire(uint32_t word, uint32_t a0, uint32_t a1);

/* ev_vis_draw (Model:02): a flash_mid site in the frame refresh's visualizer branch, after the eight tracks are
   drawn and before the frame is pushed, while the firmware's clip column (grid + 0x12EA) is still 0. That branch
   never reaches ev_draw's site. The argument is the drawing context (fw_grid): mods draw with fw_rect. */
#define EV_VIS_DRAW_W 0x0021u
void core_vis_draw(void *grid) { fire(EV_VIS_DRAW_W, (uint32_t)grid, 0); }
/* ev_vis_scope (Model:02): the visualizer's call of its per-track scope drawing comes here (a `calls` site); a
   handler that takes it skips that track's scope (CHAOS CONTROL draws its own, cheaper visualizer instead) */
#ifdef fw_vis_scope_draw
#define EV_VIS_SCOPE_W (0x0022u | 0x10000u)
void core_vis_scope(void *scope) { if (!fire(EV_VIS_SCOPE_W, (uint32_t)scope, 0)) fw_vis_scope_draw(scope); }
#endif
#ifdef FW_SIDE_0
/* core's full-screen pages (4 OPTIONS, 5 MOD LOADER) keep the firmware's right-hand column from painting over them */
static int side_hidden(void) { return ram->page_mode == 4u || ram->page_mode == 5u; }
void core_side0(void *o) { if (!side_hidden()) FW_SIDE_0(o); }
void core_side1(void *o) { if (!side_hidden()) FW_SIDE_1(o); }
void core_side2(void *o) { if (!side_hidden()) FW_SIDE_2(o); }
void core_side3(void *o) { if (!side_hidden()) FW_SIDE_3(o); }
void core_side4(void *o) { if (!side_hidden()) FW_SIDE_4(o); }
void core_side5(void *o) { if (!side_hidden()) FW_SIDE_5(o); }
void core_side6(void *o) { if (!side_hidden()) FW_SIDE_6(o); }
void core_side7(void *o) { if (!side_hidden()) FW_SIDE_7(o); }
#endif
void core_tick(void)
{
#if defined(fw_rect) && defined(fw_input_router) && defined(FW_UI_VISUALIZER)
    if (ram->lm_vis) {                              /* EDIT on CHAOS CONTROL in the MOD LOADER: the visualizer, */
        ram->lm_vis = 0;                            /* through the firmware's router (ev_key's site is its caller) */
        if (!fw_ui[FW_UI_VISUALIZER]) fw_input_router(fw_ui, 0xE4);
    }
#endif
#if DEST_EXT
    if (ram->dest_load_ok) dest_load_done();      /* an .m8i load finished: its sidecar names its destinations */
#endif
    uint32_t usb = fw_usb_configuration != 0;
    if (usb != ram->usb_last) { ram->usb_last = usb; fire(EV_USB_W, usb, 0); }
    uint32_t what = ram->song_pending;
    if (!what || !FW_SD_MOUNTED() || fw_millis - ram->song_at < SONG_WAIT) return;
    if (what == 2) {
        song_saved();
        if (ram->song_result != 1 && ++ram->song_tries < SONG_TRIES) { ram->song_at = fw_millis; return; }
        ram->song_pending = 0;
    } else {
        int busy = data_load();
        if (!busy && what == 1) busy = song_loaded();
        if (busy && ++ram->song_tries < SONG_TRIES) { ram->song_at = fw_millis; return; }
        if (busy && !ram->data_done) {               /* never readable: tell the mods there is nothing */
            ram->data_done = 1;
            for (uint32_t i = 0; i < ram->ndata; i++) if (ram->data[i]->loaded) ram->data[i]->loaded(0);
        }
        ram->song_pending = 0;
    }
}

/* ---- events core raises from C --------------------------------------------------------------------------------
 * fire() calls the dispatcher (core 0.2's, at the start of the hook bus, CORE_DISPATCH) as a site stub does:
 * ip = event | TAKE, arguments in r0-r3; it returns the taking handler's result, or 0. */
static int fire(uint32_t word, uint32_t a0, uint32_t a1)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t ip __asm__("ip") = word;
    register uint32_t fn __asm__("r3") = CORE_DISPATCH;
    __asm volatile("blx r3" : "+r"(r0), "+r"(r1), "+r"(ip), "+r"(fn) : : "r2", "lr", "memory", "cc");
    return (int)r0;
}
/* Other objects in the graph (fw.h). A send effect's input is handed over before the effect runs. */
#define EV_FX_AUDIO_W    (0x0034u | TAKE)
#define EV_PARAMS_W      0x0035u
/* A taken stereo effect (MFX, reverb) does not run: the handler's blocks go to its two outputs in its place, and
   its input queue is released and cleared as its own update would. The delay (mono in) is not replaceable. */
static int fx_in(void *obj, uint32_t stage, int stereo)
{
    uint8_t **q = *(uint8_t ***)((uint8_t *)obj + 0x20);
    if (!ram->fx_subs || !q || !q[0] || (stereo && !q[1])) return 0;
    int taken = fire3(EV_FX_AUDIO_W, stage, (uint32_t)(q[0] + 4), stereo ? (uint32_t)(q[1] + 4) : 0);
    if (!taken || !stereo) return 0;
    for (int i = 0; i < 2; i++) {
        fw_audio_transmit(obj, q[i], i);
        fw_audio_release(q[i]);
        q[i] = 0;
    }
    return 1;
}
static void core_mfx(void *obj) { if (!fx_in(obj, FW_FX_MFX, 1)) ((update_fn)ram->mfx_orig)(obj); }
static void core_delay(void *obj) { fx_in(obj, FW_FX_DELAY, 0); ((update_fn)ram->delay_orig)(obj); }
static void core_reverb(void *obj) { if (!fx_in(obj, FW_FX_REVERB, 1)) ((update_fn)ram->reverb_orig)(obj); }
static void core_clock(void *obj)
{
    if (ram->params_subs) fire3(EV_PARAMS_W, (uint32_t)FW_CLOCK_OBJ, 0, 0);
    ((update_fn)ram->clock_orig)(obj);
}

/* ev_step_edit for bulk edits: every phrase edit ends in the phrase view's +0x34 (bulk) or +0x38 (one cell; its
   entry is already ev_step_edit's site). Core keeps the phrase as it last saw it and, after a bulk redraw, reports
   the steps that changed: ev_step_edit(view, first changed step, steps from it to the last changed, -1). A redraw
   that changed nothing (a selection, a copy) says nothing. */
#define EV_STEP_EDIT_W 0x0081u
static int fire4(uint32_t word, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
static uint8_t *phrase_of(void *view) { return FW_PHRASE_STEP(((uint8_t *)view)[4], 0); }
static void snap_take(void *view)
{
    uint8_t *p = phrase_of(view);
    for (uint32_t i = 0; i < sizeof ram->snap; i++) ram->snap[i] = p[i];
    ram->snap_phrase = ((uint8_t *)view)[4];
}
static void core_phrase_bulk(void *view, uint32_t a1, uint32_t a2, uint32_t a3)
{
    ((void (*)(void *, uint32_t, uint32_t, uint32_t))ram->bulk_orig)(view, a1, a2, a3);
    uint8_t *p = phrase_of(view);
    if (ram->snap_phrase == ((uint8_t *)view)[4]) {
        int first = -1, last = -1;
        for (int s = 0; s < 16; s++)
            for (int k = 0; k < 9; k++)
                if (p[s * 9 + k] != ram->snap[s * 9 + k]) { if (first < 0) first = s; last = s; break; }
        if (first >= 0)
            fire4(EV_STEP_EDIT_W, (uint32_t)view, (uint32_t)(p + first * 9), (uint32_t)(last - first + 1), 0xFFFFFFFFu);
    }
    snap_take(view);
}
static void core_phrase_cell(void *view, uint32_t a1, uint32_t a2, uint32_t a3)
{
    ((void (*)(void *, uint32_t, uint32_t, uint32_t))ram->cell_orig)(view, a1, a2, a3);   /* ev_step_edit's site */
    snap_take(view);
}

static int fire4(uint32_t word, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    register uint32_t ip __asm__("ip") = word;
    register uint32_t fn __asm__("r4") = CORE_DISPATCH;
    __asm volatile("blx r4" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(ip), "+r"(fn) : : "lr", "memory", "cc");
    return (int)r0;
}

/* ev_sysex: usbMIDI's SysEx handler pointer (FW_SYSEX_HANDLER) is the firmware's, set in setup(); core_boot
   puts core_sysex there and keeps the firmware's to call when no handler takes the chunk. */
#define EV_SYSEX_W (0x0047u | TAKE)
static void core_sysex(const uint8_t *data, uint32_t len, uint32_t last)
{
    if (fire3(EV_SYSEX_W, (uint32_t)data, len, last)) return;
    if (ram->sysex_orig) ((void (*)(const uint8_t *, uint32_t, uint32_t))ram->sysex_orig)(data, len, last);
}

/* ev_render_in / ev_render_out: the audio library renders each block in the software interrupt (IRQ 70, vector
   86 of the RAM vector table); core_boot puts this in its place. Audio-thread handlers belong in ITCM, which is
   where loaded mods run. */
#define EV_RENDER_IN_W  0x0030u
#define EV_RENDER_OUT_W 0x0031u
#define FW_AUDIO_VECTOR (16 + 70)
static void core_audio(void)
{
    fire(EV_RENDER_IN_W, 0, 0);
    ((void (*)(void))ram->audio_orig)();
    fire(EV_RENDER_OUT_W, 0, 0);
}

/* ev_midi: usb_midi_read's call of usb_midi_read_message comes here. Packets mods played in (midi_receive) go
   first; a packet a handler takes is dropped, and the next one is read. */
#define EV_MIDI_W (0x0043u | TAKE)
uint32_t core_midi_read(void)
{
    for (;;) {
        uint32_t pk;
        if (ram->midi_head != ram->midi_tail) pk = ram->midi_q[ram->midi_tail++ % MIDI_Q];
        else pk = fw_usb_midi_read_message();
        if (!pk || !fire(EV_MIDI_W, pk, 0)) return pk;
    }
}

/* ---- the API for mods (dl/core.h) --------------------------------------------------------------------------- */
static void api_restart(void)
{
    uint32_t t = fw_millis;
    while (fw_millis - t < 50) { }                  /* let USB send what it has */
    restart_now();
}
static int api_page_add(const struct dl_page *p)
{
    if (!p || ram->npages >= MAX_PAGES) return 0;
    ram->pages[ram->npages++] = p;
    return 1;
}
static void api_page_open(const struct dl_page *p)
{
    if (!p || !page_installed()) return;
    uint32_t here = fw_ui[FW_UI_VIEW];
    ram->page_back = here == FW_VIEW_FREE ? (ram->page_mode == 2 ? ram->page_back
                                             : ram->page_mode == 3 ? PAGE_BACK_MENU
                                             : ram->page_mode == 5 ? PAGE_BACK_LOADER : PAGE_BACK_MODS) : here;
    ram->cur_page = p;
    ram->page_mode = 2;
    ram->page_dirty = 1;
    if (p->open) p->open();
    if (here != FW_VIEW_FREE) fw_set_view(fw_ui, FW_VIEW_FREE);
}
static void api_page_close(void)
{
    if (ram->page_mode != 2) return;
    ram->page_mode = 0;
    ram->cur_page = 0;
    if (ram->page_back == PAGE_BACK_MODS) {
        if (FW_SD_MOUNTED()) page_read();
        ram->page_dirty = 1;
    } else if (ram->page_back == PAGE_BACK_MENU) {
        ram->page_mode = 3;
        ram->page_dirty = 1;
    } else if (ram->page_back == PAGE_BACK_LOADER) {
        ram->page_mode = 5;
        ram->page_dirty = 1;
    } else {
        fw_set_view(fw_ui, (int)ram->page_back);
    }
}
/* ---- the Mod Menu (core-dn1's) --------------------------------------------------------------------------------
 * Every mod page as a grid, two across, then the mods' settings. UP / DOWN move a row; LEFT / RIGHT move across the
 * grid, or change the setting under the cursor; EDIT opens a page; OPT goes back. Settings are kept in
 * /dirtyloader/settings.txt as label=value lines. */
#define SETTINGS_PATH "/dirtyloader/settings.txt"
#define MENU_TOP 5

static void settings_save(void)
{
    char *const t = (char *)(SCRATCH + STATUS_SIZE), *o = t;
    for (uint32_t i = 0; i < ram->nsettings && o < t + LIST_SIZE - 32; i++) {
        o = put(o, ram->settings[i]->label);
        *o++ = '=';
        o = put_dec(o, *ram->settings[i]->value);
        *o++ = 10;
    }
    if (FW_SD_MOUNTED()) write_file(SETTINGS_PATH, t, (uint32_t)(o - t));
}

static void setting_load(const struct dl_setting *s)
{
    struct fw_file f;
    char *const t = (char *)(SCRATCH + STATUS_SIZE);
    if (!f_open(&f, SETTINGS_PATH, FW_O_READ)) return;
    int n = f_read(&f, t, LIST_SIZE - 1);
    fw_file_close(&f);
    if (n <= 0) return;
    t[n] = 0;
    for (char *p = t; *p;) {
        char *line = p;
        while (*p && *p != 10 && *p != 13) p++;
        if (*p) *p++ = 0;
        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq = 0;
        if (!seq(line, s->label)) continue;
        uint32_t v = 0;
        for (char *d = eq + 1; *d >= '0' && *d <= '9'; d++) v = v * 10 + (uint32_t)(*d - '0');
        if (v >= s->min && v <= s->max) *s->value = (uint8_t)v;
    }
}

static int api_setting_add(const struct dl_setting *s)
{
    if (!s || !s->label || !s->value || s->min > s->max || ram->nsettings >= MAX_SETTINGS) return 0;
    ram->settings[ram->nsettings++] = s;
    if (FW_SD_MOUNTED()) setting_load(s);
    return 1;
}

static uint32_t menu_rows_pages(void) { return (ram->npages + 1) / 2; }

static void menu_draw_all(void)
{
    char b[48], *o;
    for (int r = 0; r < FW_GRID_ROWS; r++) row(r, "");
    row(3, " MOD MENU");
    for (uint32_t i = 0; i < ram->npages; i++) {
        o = b;
        *o++ = ' ';
        *o++ = ram->menu_sel == i ? '>' : ' ';
        const char *t = ram->pages[i]->title ? ram->pages[i]->title : ram->pages[i]->mod;
        for (uint32_t k = 0; t && t[k] && k < 14; k++) *o++ = t[k];
        *o = 0;
        fw_text(fw_grid, (int)(i & 1) * 16, MENU_TOP + (int)(i / 2), b);
    }
    if (!ram->npages) row(MENU_TOP, "   NO MOD PAGES");
    int r = MENU_TOP + (int)(ram->npages ? menu_rows_pages() : 1) + 1;
    if (ram->nsettings) row(r++, " SETTINGS");
    for (uint32_t i = 0; i < ram->nsettings && r < 20; i++, r++) {
        const struct dl_setting *s = ram->settings[i];
        o = b;
        *o++ = ' ';
        *o++ = ram->menu_sel == ram->npages + i ? '>' : ' ';
        uint32_t k = 0;
        for (; s->label[k] && k < 16; k++) *o++ = s->label[k];
        while (k++ < 18) *o++ = ' ';
        uint8_t v = *s->value;
        if (s->names) { const char *nm = s->names[v - s->min]; for (k = 0; nm[k] && k < 12; k++) *o++ = nm[k]; }
        else o = put_dec(o, v);
        *o = 0;
        row(r, b);
    }
    row(21, " EDIT OPEN  LEFT/RIGHT CHANGE");
    row(22, " OPT BACK");
}

static void menu_show(uint32_t back)
{
    if (!page_installed()) return;
    uint32_t here = fw_ui[FW_UI_VIEW];
    ram->menu_back = here == FW_VIEW_FREE ? back : here;
    ram->menu_sel = 0;
    ram->page_mode = 3;
    ram->page_dirty = 1;
    if (here != FW_VIEW_FREE) fw_set_view(fw_ui, FW_VIEW_FREE);
}
static void api_menu_open(void) { menu_show(PAGE_BACK_MODS); }

static void menu_input(int action)
{
    uint32_t np = ram->npages, n = np + ram->nsettings, sel = ram->menu_sel;
    if (action == FW_KEY_OPT) {
        ram->page_mode = 0;
        if (ram->menu_back == PAGE_BACK_MODS) { if (FW_SD_MOUNTED()) page_read(); ram->page_dirty = 1; }
        else fw_set_view(fw_ui, (int)ram->menu_back);
        return;
    }
    if (sel < np) {                                 /* the grid */
        if (action == FW_KEY_EDIT) { api_page_open(ram->pages[sel]); return; }
        if (action == FW_KEY_RIGHT && (sel & 1) == 0 && sel + 1 < np) sel++;
        else if (action == FW_KEY_LEFT && (sel & 1)) sel--;
        else if (action == FW_KEY_DOWN) sel = sel + 2 < np ? sel + 2 : (ram->nsettings ? np : sel);
        else if (action == FW_KEY_UP && sel >= 2) sel -= 2;
    } else if (sel < n) {                           /* a setting */
        const struct dl_setting *s = ram->settings[sel - np];
        uint8_t v = *s->value;
        if (action == FW_KEY_RIGHT && v < s->max) v++;
        else if (action == FW_KEY_LEFT && v > s->min) v--;
        else if (action == FW_KEY_DOWN && sel + 1 < n) sel++;
        else if (action == FW_KEY_UP) sel = sel > np ? sel - 1 : (np ? ((np - 1) & ~1u) : sel);
        if (v != *s->value) { *s->value = v; settings_save(); if (s->changed) s->changed(); }
    }
    ram->menu_sel = sel;
    ram->page_dirty = 1;
}

/* ---- the machine pages (core 0.7) -----------------------------------------------------------------------------
 * LEFT+RIGHT, on any screen but the visualizer: a page per mod, its settings as up to eight knobs in two rows of four,
 * in the manner of Elektron's machine pages (and Elekloader's machine-pages). Arrows pick a knob; EDIT+LEFT/RIGHT
 * turn it by 1, EDIT+UP/DOWN by 4; SHIFT+LEFT/RIGHT flip pages; OPT or LEFT+RIGHT leave. A setting belongs to the mod
 * whose code holds its dl_setting (they are const); a mod with more than eight gets more pages. On the Model:02 each
 * knob is a 270-degree arc of dots drawn with the firmware's fill-rect, one frame after the text (so the text's own
 * repaint doesn't cover it), and only again when its value or the selection changes. Headless shows a text bar. */
#define ACTION_LEFT_RIGHT 0x84
#define MP_MAX 8
#ifdef fw_rect
#include "dl/pixui.h"
static const int8_t mp_dots[21][2] = {
    {-18,18}, {-22,14}, {-25,8}, {-26,2}, {-26,-4}, {-24,-10}, {-21,-15}, {-17,-20}, {-12,-23}, {-6,-25}, {0,-26},
    {6,-25}, {12,-23}, {17,-20}, {21,-15}, {24,-10}, {26,-4}, {26,2}, {25,8}, {22,14}, {18,18} };
#endif

static int setting_owner(const struct dl_setting *s)
{
    uint32_t a = (uint32_t)s;
    for (uint32_t i = 0; i < ram->nmods; i++)
        if (a >= ram->mod_lo[i] && a < ram->mod_hi[i]) return (int)i;
    return -1;
}
/* page k's settings (up to 8) and its mod (0: settings no mod's code holds); returns how many pages there are */
static uint32_t mp_page_of(uint32_t k, const struct dl_setting **out, uint32_t *n, const struct dl_mod **mod)
{
    uint32_t pages = 0;
    *n = 0;
    for (int mi = 0; mi <= (int)ram->nmods; mi++) {
        int who = mi < (int)ram->nmods ? mi : -1;
        uint32_t c = 0;
        for (uint32_t i = 0; i < ram->nsettings; i++) {
            if (setting_owner(ram->settings[i]) != who) continue;
            if (pages == k) { out[c] = ram->settings[i]; *mod = who >= 0 ? ram->mods[who] : 0; *n = c + 1; }
            if (++c == MP_MAX) { pages++; c = 0; }
        }
        if (c) pages++;
    }
    return pages;
}
static char *mp_title(char *o, const struct dl_mod *m)
{
    const char *t = m ? (m->title ? m->title : m->id) : "OTHER";
    for (uint32_t k = 0; t[k] && t[k] != ':' && k < 22; k++) *o++ = t[k] >= 'a' && t[k] <= 'z' ? (char)(t[k] - 32) : t[k];
    return o;
}
/* a knob's label: the setting's, less the mod's first word ("CHAOS FX" on CHAOS CONTROL's page shows "FX") */
static const char *mp_label(const struct dl_setting *s, const struct dl_mod *m)
{
    const char *l = s->label, *t = m && m->title ? m->title : "";
    uint32_t k = 0;
    while (t[k] && t[k] != ' ' && t[k] != ':' && l[k] && (l[k] == t[k] || l[k] == t[k] - 32 || l[k] - 32 == t[k])) k++;
    return k && l[k] == ' ' && (!t[k] || t[k] == ' ' || t[k] == ':') ? l + k + 1 : l;
}
#ifndef fw_rect                                     /* headless: the machine pages in the text grid */
static void mp_center(int r, int col, const char *s, int sel)
{
    char b[12];
    uint32_t n = 0;
#ifdef fw_rect
    (void)sel;                                      /* the selected knob is drawn white */
    while (s[n] && n < 8) n++;
    int pad = (int)(8 - n) / 2;
#else
    while (s[n] && n < 6) n++;
    int pad = (int)(8 - n - (sel ? 2 : 0)) / 2;
#endif
    char *o;
    for (int i = 0; i < 8; i++) b[i] = ' ';
    o = b + (pad > 0 ? pad : 0);
#ifndef fw_rect
    if (sel) *o++ = '>';
#endif
    for (uint32_t i = 0; i < n; i++) *o++ = s[i];
#ifndef fw_rect
    if (sel) *o++ = '<';
#endif
    b[8] = 0;
    fw_text(fw_grid, col * 8, r, b);
}
#endif
static void mp_value(char *b, const struct dl_setting *s)
{
    uint8_t v = *s->value;
    if (s->names) { const char *nm = s->names[v - s->min]; uint32_t k = 0; for (; nm[k] && k < 8; k++) b[k] = nm[k]; b[k] = 0; }
    else *put_dec(b, v) = 0;
}
#define MP_ROW(g)    (5 + (g) * 8)                  /* a knob row's label row; its knob below, its value at +6 */
#ifndef fw_rect
static void mp_draw_text(void)
{
    const struct dl_setting *s[MP_MAX];
    const struct dl_mod *m = 0;
    uint32_t n, pages = mp_page_of(ram->mp_page, s, &n, &m);
    char b[48], *o;
    for (int r = 0; r < 23; r++) row(r, "");
    o = put(b, " ");
    o = mp_title(o, m);
    if (pages > 1) { o = put(o, "  "); o = put_dec(o, ram->mp_page + 1); *o++ = '/'; o = put_dec(o, pages); }
    *o = 0;
    row(3, pages ? b : " MOD PAGES: NO MOD SETTINGS");
    for (uint32_t i = 0; i < n; i++) {
        int g = (int)i / 4, c = (int)i % 4;
        mp_center(MP_ROW(g), c, mp_label(s[i], m), ram->mp_sel == i);
        mp_value(b, s[i]);
        mp_center(MP_ROW(g) + 6, c, b, 0);
#ifndef fw_rect
        uint32_t span = s[i]->max - s[i]->min, fill = span ? (uint32_t)(*s[i]->value - s[i]->min) * 6u / span : 6;
        o = put(b, "[");                            /* 8 wide: a knob's column */
        for (uint32_t k = 0; k < 6; k++) *o++ = k < fill ? '#' : '.';
        *o++ = ']'; *o = 0;
        fw_text(fw_grid, c * 8, MP_ROW(g) + 3, b);
#endif
    }
    row(21, " ARROWS PICK  EDIT+</> TURN");
    row(22, " SHIFT+</> MOD  OPT BACK");
    for (int i = 0; i < MP_MAX; i++) ram->mp_drawn[i] = 0;
    ram->mp_gfx = 2;                                /* the knobs, the frame after next */
}
#endif
#ifdef fw_rect
/* The Model:02's OPTIONS page, in the MOD LOADER's style: the mod's name in a purple header with the page, eight knob
   cells (label, an arc of dots, value; the selected one in brackets) and the keys at the foot, all drawn with
   dl/pixui.h across the whole screen (the firmware's right-hand column held off: core_side*), one piece a frame, each again only when it changes. */
#define MPX(c)    (4 + (c) * 119)
#define MPY(g)    (42 + (g) * 122)
#define MP_D_HEAD 1u
#define MP_D_FOOT 2u
#define MP_D_KNOB(i) (4u << (i))
#define MP_D_ALL  0x3FFu
static void mp_rect(int x, int y, int w, int h, uint16_t c) { if (w > 0 && h > 0) fw_rect(fw_grid, x, y, w, h, c); }
static void mp_g_head(const struct dl_mod *m, uint32_t pages)
{
    char b[28], *o = mp_title(b, m);
    *o = 0;
    mp_rect(4, 4, 472, 34, PUI_BG);
    pui_fill(mp_rect, 4, 4, 280, 32, PUI_PURPLE);
    int sc = pui_width(b, 3) <= 266 ? 3 : 2;
    pui_text(mp_rect, 10, sc == 3 ? 9 : 13, b, sc, PUI_BG);
    pui_box(mp_rect, 290, 4, 186, 32, 2, PUI_PURPLE);
    o = b; *o++ = '}'; *o++ = ' ';
    o = put_dec(o, ram->mp_page + 1); *o++ = '/'; o = put_dec(o, pages ? pages : 1);
    *o++ = ' '; *o++ = '{'; *o = 0;
    pui_text(mp_rect, 290 + (186 - pui_width(b, 2)) / 2, 13, b, 2, PUI_CREAM);
}
static void mp_g_knob(int i, const struct dl_setting *s, const struct dl_mod *m, int sel)
{
    int x = MPX(i % 4), y = MPY(i / 4);
    mp_rect(x, y, 117, 120, PUI_BG);
    if (!s) return;
    char b[12];
    const char *l = mp_label(s, m);
    uint32_t k = 0;
    for (; l[k] && k < 8; k++) b[k] = l[k];
    b[k] = 0;
    pui_text(mp_rect, x + (117 - pui_width(b, 2)) / 2, y + 6, b, 2, PUI_CREAM);
    int cx = x + 58, cy = y + 58;
    uint32_t span = s->max - s->min, lit = span ? (uint32_t)(*s->value - s->min) * 20u / span : 20;
    uint16_t on = sel ? PUI_CREAM : PUI_PURPLE;
    for (uint32_t d = 0; d < 21; d++)
        mp_rect(cx + mp_dots[d][0] - 2, cy + mp_dots[d][1] - 2, d == lit ? 6 : 4, d == lit ? 6 : 4,
                d <= lit ? on : PUI_DPURPLE);
    pui_fill(mp_rect, cx - 5, cy - 5, 11, 11, sel ? PUI_CREAM : PUI_DPURPLE);   /* the hub */
    mp_value(b, s);
    pui_text(mp_rect, x + (117 - pui_width(b, 2)) / 2, y + 96, b, 2, sel ? PUI_PURPLE : PUI_CREAM);
    if (sel) pui_brackets(mp_rect, x + 1, y + 1, 115, 118, 12, 3, PUI_PURPLE);
}
static void mp_g_foot(void)
{
    mp_rect(4, 288, 472, 31, PUI_BG);
    pui_box(mp_rect, 4, 289, 472, 30, 2, PUI_PURPLE);
    static const char *const key[4] = { "{}", "E{}", "S}", "OPT" }, *const what[4] = { "PICK", "TURN", "MOD", "BACK" };
    int x = 10;
    for (int i = 0; i < 4; i++) {
        int kw = pui_width(key[i], 2);
        pui_fill(mp_rect, x, 294, kw + 10, 20, PUI_PURPLE);
        pui_text(mp_rect, x + 5, 297, key[i], 2, PUI_BG);
        x = pui_text(mp_rect, x + kw + 15, 297, what[i], 2, PUI_CREAM) + 6;
    }
}
static void mp_draw(void)
{
    if (ram->page_dirty) {                          /* a new page: blank the grid once, draw after its repaint */
        ram->page_dirty = 0;
        for (int r = 0; r < 23; r++) row(r, "");
        ram->mp_gfx = 2;
        ram->mp_dirty = MP_D_ALL;
        return;
    }
    if (ram->mp_gfx && --ram->mp_gfx) return;
    if (!ram->mp_dirty) return;
    const struct dl_setting *s[MP_MAX];
    const struct dl_mod *m = 0;
    uint32_t n, pages = mp_page_of(ram->mp_page, s, &n, &m);
    volatile uint16_t *clip = (volatile uint16_t *)(fw_grid + 0x12EA);
    uint16_t was = *clip;
    *clip = 0;
    if (ram->mp_dirty == MP_D_ALL) mp_rect(0, 0, FW_SCREEN_W, 320, PUI_BG);
    for (uint32_t bit = 0; bit < 10; bit++) {
        uint32_t b = 1u << bit;
        if (!(ram->mp_dirty & b)) continue;
        ram->mp_dirty &= ~b;
        if (b == MP_D_HEAD) { mp_g_head(m, pages); continue; }   /* the header goes with the next piece */
        if (b == MP_D_FOOT) mp_g_foot();
        else {
            uint32_t i = bit - 2;
            mp_g_knob((int)i, i < n ? s[i] : 0, m, ram->mp_sel == i);
            if (!n && !i) pui_text(mp_rect, (FW_SCREEN_W - pui_width("NO SETTINGS", 2)) / 2, 150, "NO SETTINGS", 2, PUI_DIM);
        }
        break;
    }
    *clip = was;
}
#else
static void mp_draw(void)
{
    if (ram->page_dirty) { ram->page_dirty = 0; mp_draw_text(); }
}
#endif
static void mp_show(void)
{
    if (!page_installed()) return;
    uint32_t here = fw_ui[FW_UI_VIEW];
    ram->mp_prev = here == FW_VIEW_FREE ? ram->page_mode : 0xFFu;
    ram->mp_back = here;
    const struct dl_setting *s[MP_MAX];
    const struct dl_mod *m;
    uint32_t n, pages = mp_page_of(0, s, &n, &m);
    if (ram->mp_page >= pages) ram->mp_page = 0;    /* ITCM keeps the last boot's: anything */
    ram->mp_sel = 0;
    ram->page_mode = 4;
    ram->page_dirty = 1;
    if (here != FW_VIEW_FREE) fw_set_view(fw_ui, FW_VIEW_FREE);
#ifdef fw_rect
    {                                               /* the screen before may have drawn more than text (a cursor) */
        volatile uint16_t *clip = (volatile uint16_t *)(fw_grid + 0x12EA);
        uint16_t was = *clip;
        *clip = 0;
        fw_rect(fw_grid, 0, 30, FW_SCREEN_W, 290, FW_RGB(0, 0, 0));
        *clip = was;
    }
#endif
}
static void mp_close(void)
{
#ifdef fw_rect
    {                                               /* hand the screen back without the knobs */
        volatile uint16_t *clip = (volatile uint16_t *)(fw_grid + 0x12EA);
        uint16_t was = *clip;
        *clip = 0;
        fw_rect(fw_grid, 0, 30, FW_SCREEN_W, 290, FW_RGB(0, 0, 0));
        *clip = was;
    }
#endif
    if (ram->mp_prev != 0xFFu) {                    /* back to core's own screen it was opened over */
        ram->page_mode = ram->mp_prev;
        if (!ram->page_mode && FW_SD_MOUNTED()) page_read();
        ram->page_dirty = 1;
    } else {
        ram->page_mode = 0;
        fw_set_view(fw_ui, (int)ram->mp_back);
    }
}
static void mp_input(int action)
{
    const struct dl_setting *s[MP_MAX];
    const struct dl_mod *m = 0;
    uint32_t n, pages = mp_page_of(ram->mp_page, s, &n, &m), sel = ram->mp_sel;
    if (action == FW_KEY_OPT) { mp_close(); return; }
    if (action == (FW_KEY_SHIFT | FW_KEY_RIGHT) || action == (FW_KEY_SHIFT | FW_KEY_LEFT)) {
        if (pages) ram->mp_page = (ram->mp_page + (action & FW_KEY_RIGHT ? 1 : pages - 1)) % pages;
        ram->mp_sel = 0;
        ram->page_dirty = 1;
        return;
    }
    if (!n) return;
    if (action & FW_KEY_EDIT) {
        const struct dl_setting *t = s[sel < n ? sel : 0];
        int v = *t->value, step = action & (FW_KEY_UP | FW_KEY_DOWN) ? 4 : 1;
        if (action & (FW_KEY_RIGHT | FW_KEY_UP)) v += step;
        else if (action & (FW_KEY_LEFT | FW_KEY_DOWN)) v -= step;
        else return;
        if (v < t->min) v = t->min;
        if (v > t->max) v = t->max;
        if (v != *t->value) {
            *t->value = (uint8_t)v;
            settings_save();
            if (t->changed) t->changed();
#ifdef fw_rect
            ram->mp_dirty |= MP_D_KNOB(sel);
#else
            char b[12];
            mp_value(b, t);
            mp_center(MP_ROW((int)sel / 4) + 6, (int)sel % 4, b, 0);
#endif
        }
        return;
    }
    uint32_t was = sel;
    if (action == FW_KEY_LEFT && sel % 4) sel--;
    else if (action == FW_KEY_RIGHT && sel % 4 < 3 && sel + 1 < n) sel++;
    else if (action == FW_KEY_UP && sel >= 4) sel -= 4;
    else if (action == FW_KEY_DOWN && sel + 4 < n) sel += 4;
    if (sel != was) {
        ram->mp_sel = sel;
#ifdef fw_rect
        ram->mp_dirty |= MP_D_KNOB(was) | MP_D_KNOB(sel);
#else
        mp_center(MP_ROW((int)was / 4), (int)was % 4, mp_label(s[was], m), 0);
        mp_center(MP_ROW((int)sel / 4), (int)sel % 4, mp_label(s[sel], m), 1);
#endif
    }
}

/* ---- the MOD LOADER (core 0.7, Model:02): all four directions ----------------------------------------------------
 * After the concept art: a header (MOD LOADER, striped, and how many mods are on of how many), tabs (ALL INST FX
 * SEQ UTIL), the mods on the card as cards two across and four down (an icon, the name, the category, a little
 * picture of it; the selected one boxed in purple with brackets), and the keys at the foot. Drawn with fill-rects and
 * dl/pixui.h in the left 34 columns (the firmware keeps its own column at the right on core's screen), one piece a
 * frame, each piece only again when it changes: moving the selection redraws two cards' frames.
 *   arrows       move
 *   EDIT         open the mod: its screen (the visualizer for CHAOS CONTROL, or the page it added), else its
 *                settings, else its INFO page
 *   SHIFT+EDIT   the mod on / off (from the next RESTART, as on the MODS page)
 *   SHIFT+RIGHT  OPTIONS: its settings as knobs (the machine pages)
 *   SHIFT+LEFT   INFO: who made it, what it does (DL_INFO, read from its .m8x), whether it runs, its file
 *   SHIFT+UP/DOWN the tab
 *   OPT          back; if mods were switched since the boot, RESTART TO APPLY? first (EDIT restarts, OPT later) */
#ifdef fw_rect
#include "dl/pixui.h"
enum { LM_ALL, LM_INST, LM_FX, LM_SEQ, LM_UTIL, LM_TABS };
static const char *const lm_tab_names[LM_TABS] = { "ALL", "INST", "FX", "SEQ", "UTIL" };
/* vis: the mod's screen is the firmware's visualizer (EDIT opens it while the mod runs) */
static const struct { const char *stem, *name; uint8_t cat, icon, vis; } lm_known[] = {
    { "chaos-control", "CHAOS CONTROL", LM_FX, PUI_H_GEM, 1 },    { "lfo-sends", "LFO SENDS", LM_INST, PUI_H_WAVE, 0 },
    { "perf-mon", "PERF MON", LM_UTIL, PUI_H_BARS, 0 },       { "load-gen", "LOAD GEN", LM_UTIL, PUI_H_CUBE, 0 },
    { "flash-probe", "FLASH TEST", LM_UTIL, PUI_H_CUBE, 0 },  { "core-api-demo", "API DEMO", LM_UTIL, PUI_H_GEAR, 0 },
    { "core-selftest", "SELFTEST", LM_UTIL, PUI_H_GEAR, 0 },  { "sd-heartbeat", "HEARTBEAT", LM_UTIL, PUI_H_HEART, 0 },
    { "audio-tone", "AUDIO TONE", LM_INST, PUI_H_WAVE, 0 },   { "track-silence", "SILENCE", LM_FX, PUI_H_RINGS, 0 },
    { "fx-replace", "FX REPLACE", LM_FX, PUI_H_DOTS, 0 },
};
#define LM_ANIM_MS 70u
enum { LM_CARDS, LM_INFO, LM_ASK };
#define LM_PER  8
#define LM_D_HEAD 1u
#define LM_D_TABS 2u
#define LM_D_FOOT 4u
#define LM_D_CARD(i) (8u << (i))
#define LM_D_ALL  0x7FFu
#define LM_D_BODY LM_D_TABS                         /* LM_INFO: the tabs and cards are one piece, the page */
#define LM_D_ASK  0x800u                            /* LM_ASK: the box, over the cards */
static void lm_rect(int x, int y, int w, int h, uint16_t c)
{
    if (w > 0 && h > 0) fw_rect(fw_grid, x, y, w, h, c);
}
static int lm_known_of(const char *file)            /* lm_known's entry for an .m8x file, or -1 */
{
    for (uint32_t k = 0; k < sizeof lm_known / sizeof lm_known[0]; k++) {
        const char *s = lm_known[k].stem;
        uint32_t i = 0;
        while (s[i] && file[i] == s[i]) i++;
        if (!s[i] && (file[i] == '.' || !file[i])) return (int)k;
    }
    return -1;
}
static uint8_t lm_cat(uint32_t i) { int k = lm_known_of(ram->page[i].name); return k < 0 ? LM_UTIL : lm_known[k].cat; }
/* the mods the tab shows, in order: their indices in ram->page */
static uint32_t lm_list(uint32_t *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < ram->page_n; i++)
        if (ram->lm_tab == LM_ALL || lm_cat(i) == ram->lm_tab) out[n++] = i;
    return n;
}
static void lm_name(char *b, uint32_t i)
{
    int k = lm_known_of(ram->page[i].name);
    const char *s = k >= 0 ? lm_known[k].name : ram->page[i].name;
    uint32_t n = 0;
    for (; s[n] && s[n] != '.' && n < 13; n++) b[n] = s[n] == '-' ? ' ' : s[n];   /* 13 fit a card */
    b[n] = 0;
}
#define LM_CX(c)  (4 + (c) * 238)
#define LM_CY(r)  (70 + (r) * 54)
#define LM_CW     234
#define LM_CH     50
static void lm_card_frame(int slot, int sel)
{
    int x = LM_CX(slot % 2), y = LM_CY(slot / 2);
    pui_box(lm_rect, x, y, LM_CW, LM_CH, 3, sel ? PUI_PURPLE : PUI_BG);
    pui_box(lm_rect, x + (sel ? 0 : 1), y + (sel ? 0 : 1), LM_CW - (sel ? 0 : 2), LM_CH - (sel ? 0 : 2), 2,
            sel ? PUI_PURPLE : PUI_CREAM);
    pui_brackets(lm_rect, x + 5, y + 5, LM_CW - 10, LM_CH - 10, 7, 2, sel ? PUI_CREAM : PUI_BG);
}
static void lm_card(int slot, int which)            /* which: an index into ram->page, or -1 for an empty slot */
{
    int x = LM_CX(slot % 2), y = LM_CY(slot / 2);
    lm_rect(x, y, LM_CW, LM_CH, PUI_BG);
    if (which < 0) return;
    int k = lm_known_of(ram->page[which].name);
    uint8_t cat = k < 0 ? LM_UTIL : lm_known[k].cat, on = ram->page[which].on;
    pui_hicon(lm_rect, x + 12, y + 4, k < 0 ? PUI_H_CUBE : lm_known[k].icon, 2, 0, PUI_BG);
    char b[16];
    lm_name(b, (uint32_t)which);
    pui_text(lm_rect, x + 58, y + 7, b, 2, on ? PUI_CREAM : PUI_DIM);
    const char *t = lm_tab_names[cat];
    int tw = pui_width(t, 2);
    pui_fill(lm_rect, x + 58, y + 27, tw + 10, 18, on ? PUI_PURPLE : PUI_DPURPLE);
    pui_text(lm_rect, x + 63, y + 29, t, 2, PUI_BG);
    int gx = x + 58 + tw + 18, gy = y + 27, gw = x + LM_CW - 8 - gx;   /* the little picture */
    uint16_t c = on ? PUI_PURPLE : PUI_DPURPLE;
    if (cat == LM_FX)                               /* a spectrum */
        for (int i = 0, xx = gx; xx + 2 < gx + gw; i++, xx += 5) {
            int h = 3 + (i * 7 + (i >> 1) * 5) % 13;
            lm_rect(xx, gy + 18 - h, 3, h, c);
        }
    else if (cat == LM_INST) {                      /* a sine */
        static const int8_t sn[8] = { 0, 5, 7, 5, 0, -5, -7, -5 };
        for (int xx = 0; xx < gw - 3; xx += 3) lm_rect(gx + xx, gy + 8 + sn[(xx / 3) % 8], 3, 3, c);
    } else if (cat == LM_SEQ)
        for (int s = 0, xx = gx; xx + 9 < gx + gw; s++, xx += 12)
            (s % 3 == 1) ? lm_rect(xx, gy + 4, 9, 9, c) : pui_box(lm_rect, xx, gy + 4, 9, 9, 2, c);
    else                                            /* signal bars */
        for (int s = 0; s < 4; s++) lm_rect(gx + gw - 32 + s * 8, gy + 14 - s * 4, 6, 4 + s * 4, c);
}
static void lm_head(void)
{
    lm_rect(4, 4, 472, 32, PUI_BG);
    pui_fill(lm_rect, 4, 4, 280, 32, PUI_PURPLE);
    pui_text(lm_rect, 10, 9, "MOD LOADER", 3, PUI_BG);
    pui_stripes(lm_rect, 238, 4, 32, 3, 12, PUI_BG);
    pui_box(lm_rect, 290, 4, 186, 32, 2, PUI_PURPLE);
    if (ram->skipped) {
        pui_text(lm_rect, 301, 8, "SAFE MODE", 2, PUI_CREAM);
        pui_text(lm_rect, 301, 23, "MODS NOT LOADED", 1, PUI_CREAM);
        return;
    }
    uint32_t on = 0;
    for (uint32_t i = 0; i < ram->page_n; i++) on += ram->page[i].on;
    char b[12], *o = b;
    *o++ = '}'; *o++ = ' ';
    *o++ = (char)('0' + on / 10 % 10); *o++ = (char)('0' + on % 10); *o++ = '/';
    *o++ = (char)('0' + ram->page_n / 10 % 10); *o++ = (char)('0' + ram->page_n % 10);
    *o++ = ' '; *o++ = '{'; *o = 0;
    pui_text(lm_rect, 290 + (186 - pui_width(b, 2)) / 2, 13, b, 2, PUI_CREAM);
}
static void lm_tabs(void)
{
    lm_rect(4, 40, 472, 26, PUI_BG);
    for (int k = 0; k < LM_TABS; k++) {
        int x = 4 + k * 84, sel = (uint32_t)k == ram->lm_tab;
        if (sel) pui_fill(lm_rect, x, 42, 80, 22, PUI_PURPLE);
        else pui_box(lm_rect, x, 42, 80, 22, 2, PUI_CREAM);
        pui_text(lm_rect, x + (80 - pui_width(lm_tab_names[k], 2)) / 2, 46, lm_tab_names[k], 2, sel ? PUI_BG : PUI_CREAM);
    }
    pui_box(lm_rect, 428, 42, 48, 22, 2, PUI_CREAM);            /* the magnifier */
    pui_box(lm_rect, 442, 45, 13, 13, 2, PUI_CREAM);
    lm_rect(455, 57, 3, 3, PUI_CREAM); lm_rect(458, 60, 3, 3, PUI_CREAM);
}
/* the selected mod, if its screen is the visualizer and it runs now (EDIT opens it; SHIFT+EDIT switches it) */
static int lm_opens(uint32_t file)
{
    int k = lm_known_of(ram->page[file].name);
#ifdef fw_input_router
    return k >= 0 && lm_known[k].vis && mod_of_file(ram->page[file].name) != 0;
#else
    (void)k; return 0;
#endif
}
static int lm_sel_file(void)                        /* the selected mod's index in ram->page, or -1 */
{
    uint32_t list[MAX_MODS], n = lm_list(list);
    return n && ram->lm_sel < n ? (int)list[ram->lm_sel] : -1;
}
/* a running mod's settings: how many, and the first machine page that holds them */
static uint32_t lm_knobs(const struct dl_mod *m, uint32_t *first)
{
    const struct dl_setting *s[MP_MAX];
    const struct dl_mod *pm;
    uint32_t k, cnt = 0, pages = m ? mp_page_of(0, s, &k, &pm) : 0;
    for (uint32_t p = 0; p < pages; p++) {
        mp_page_of(p, s, &k, &pm);
        if (pm != m) continue;
        if (!cnt && first) *first = p;
        cnt += k;
    }
    return cnt;
}
static void lm_foot(void)
{
    lm_rect(4, 288, 472, 31, PUI_BG);
    pui_box(lm_rect, 4, 289, 472, 30, 2, PUI_PURPLE);
    if (ram->skipped) {
        const char *help = ram->guard_recovery == GUARD_CLEARED ? "REBOOT TO RE-ENABLE MODS   OPT BACK"
                         : ram->guard_recovery == GUARD_WRITE_ERROR ? "SD: SET DIRTYLOADER/GUARD TO GOOD"
                         : "PRESS PLAY TO EXIT SAFE MODE";
        pui_text(lm_rect, 10, 297, help, 2, PUI_CREAM);
        return;
    }
    static const char *const kc[5] = { "E", "SE", "S}", "S{", "OPT" }, *const ki[4] = { "E", "SE", "S}", "OPT" },
                             *const ka[2] = { "E", "OPT" };
    const char *wc[5] = { "OPEN", "OFF", "OPTS", "INFO", "BACK" }, *wi[4] = { "OPEN", "OFF", "OPTIONS", "BACK" },
               *const wa[2] = { "RESTART NOW", "LATER" };
    int f = lm_sel_file(), cnt;
    if (f >= 0 && !ram->page[f].on) wc[1] = wi[1] = "ON";
    const char *const *key, *const *what;
    if (ram->lm_mode == LM_ASK) { key = ka; what = wa; cnt = 2; }
    else if (ram->lm_mode == LM_INFO) { key = ki; what = wi; cnt = 4; }
    else { key = kc; what = wc; cnt = 5; }
    ram->lm_foot_open = f >= 0 ? ram->page[f].on : 0u;   /* the ON / OFF it shows */
    int x = 10;
    for (int i = 0; i < cnt; i++) {
        int kw = pui_width(key[i], 2);
        pui_fill(lm_rect, x, 294, kw + 10, 20, PUI_PURPLE);
        pui_text(lm_rect, x + 5, 297, key[i], 2, PUI_BG);
        x = pui_text(lm_rect, x + kw + 16, 297, what[i], 2, PUI_CREAM) + 8;
    }
}

/* ---- INFO: read from the mod's .m8x on the card (so a mod that is off has one too). The image is linked at 0, so
   its pointers are offsets into the image, which starts right after the .m8x header. */
struct lm_about {
    char version[12], author[28], about[232];
    uint32_t size, hooks, found;
};
static void lm_fstr(struct fw_file *f, uint32_t base, uint32_t image, uint32_t at, char *out, uint32_t n)
{
    out[0] = 0;
    if (!at || at >= image || !f_seek(f, base + at)) return;
    int k = f_read(f, out, n - 1);
    out[k > 0 ? k : 0] = 0;
}
static void lm_about_read(const char *name, struct lm_about *a)
{
    zero(a, sizeof *a);
    char path[96];
    char *o = put(path, "/dirtyloader/mods/");
    for (uint32_t i = 0; name[i] && o < path + sizeof path - 1; i++) *o++ = name[i];
    *o = 0;
    struct fw_file f;
    struct m8x h;
    struct dl_mod m;
    if (!f_open(&f, path, FW_O_READ)) return;
    uint32_t base = 0;
    if (read_header(&f, &h) && h.image >= sizeof m && read_exact(&f, &m, sizeof m) && m.magic == DL_MOD_MAGIC) {
        base = h.magic == M8X_MAGIC2 ? M8X_HEAD1 + 4u : M8X_HEAD1;
        a->found = 1;
        a->size = h.image + h.bss;
        uint32_t subs = (uint32_t)m.subs, info = 0;
        for (uint32_t i = 0; i < m.nsubs && i < 64u; i++) {
            struct dl_sub s;
            if (subs + 8u * i + 8u > h.image || !f_seek(&f, base + subs + 8u * i) || !read_exact(&f, &s, sizeof s)) break;
            if (s.event == EV_INFO) info = (uint32_t)s.fn;
            else a->hooks++;
        }
        lm_fstr(&f, base, h.image, (uint32_t)m.version, a->version, sizeof a->version);
        struct dl_info in = { 0, 0 };
        if (info && info + sizeof in <= h.image && f_seek(&f, base + info) && read_exact(&f, &in, sizeof in)) {
            lm_fstr(&f, base, h.image, (uint32_t)in.author, a->author, sizeof a->author);
            lm_fstr(&f, base, h.image, (uint32_t)in.about, a->about, sizeof a->about);
        }
        if (!a->about[0]) lm_fstr(&f, base, h.image, (uint32_t)m.title, a->about, sizeof a->about);   /* no DL_INFO */
    }
    fw_file_close(&f);
}
/* text in lines of up to cols characters, broken at spaces; returns the lines drawn */
static int lm_wrap(const char *s, int x, int y, int cols, int lines, uint16_t c)
{
    char ln[48];
    int r = 0;
    if (cols > 47) cols = 47;
    while (*s && r < lines) {
        while (*s == ' ') s++;
        int n = 0, cut = 0;
        while (s[n] && s[n] != '\n' && n < cols) { if (s[n] == ' ') cut = n; n++; }
        if (s[n] && s[n] != ' ' && s[n] != '\n' && cut) n = cut;
        for (int i = 0; i < n; i++) ln[i] = s[i];
        ln[n] = 0;
        pui_text(lm_rect, x, y + r * 18, ln, 2, c);
        s += n;
        if (*s == '\n') s++;
        r++;
    }
    return r;
}
static void lm_info_draw(void)
{
    lm_rect(4, 40, 472, 246, PUI_BG);
    int f = lm_sel_file();
    if (f < 0) return;
    const char *nm = ram->page[f].name;
    struct lm_about a;
    lm_about_read(nm, &a);
    int k = lm_known_of(nm);
    uint8_t cat = k < 0 ? LM_UTIL : lm_known[k].cat;
    pui_hicon(lm_rect, 12, 44, k < 0 ? PUI_H_CUBE : lm_known[k].icon, 2, 0, PUI_BG);
    char b[64], *o;
    lm_name(b, (uint32_t)f);
    pui_text(lm_rect, 62, 46, b, 3, PUI_CREAM);
    const char *t = lm_tab_names[cat];
    int tw = pui_width(t, 2);
    pui_fill(lm_rect, 470 - tw - 10, 46, tw + 10, 18, PUI_PURPLE);
    pui_text(lm_rect, 470 - tw - 5, 48, t, 2, PUI_BG);
    o = b;
    if (a.version[0]) { o = put(o, "V"); o = put(o, a.version); }
    if (a.author[0]) { o = put(o, o > b ? "  BY " : "BY "); o = put(o, a.author); }
    *o = 0;
    pui_text(lm_rect, 62, 74, b, 2, PUI_DIM);
    pui_fill(lm_rect, 8, 96, 464, 2, PUI_DPURPLE);
    if (a.found) lm_wrap(a.about, 10, 104, 38, 6, PUI_CREAM);
    else pui_text(lm_rect, 10, 104, "CAN'T READ THIS MOD'S FILE", 2, PUI_MAGENTA);
    pui_fill(lm_rect, 8, 214, 464, 2, PUI_DPURPLE);

    /* whether it runs: now, and from the next RESTART */
    int e = boot_err(nm), on = ram->page[f].on;
    uint16_t c = PUI_CREAM;
    o = b;                                          /* each line at most 38 characters: the screen's width */
    if (ram->page[f].check) { o = put(o, "WON'T LOAD: "); o = put(o, ERRORS[ram->page[f].check]); c = PUI_MAGENTA; }
    else if (e > 0) { o = put(o, "REFUSED: "); o = put(o, ERRORS[e]); c = PUI_MAGENTA; }
    else if (e < 0 && on && ram->skipped) { o = put(o, "STATUS  SKIPPED (CRASH GUARD)"); c = PUI_MAGENTA; }
    else if (!e) { o = put(o, on ? "STATUS  ON" : "STATUS  ON, OFF FROM RESTART"); c = on ? PUI_CYAN : PUI_ORANGE; }
    else { o = put(o, on ? "STATUS  OFF, ON FROM RESTART" : "STATUS  OFF"); c = on ? PUI_ORANGE : PUI_DIM; }
    *o = 0;
    b[38] = 0;
    pui_text(lm_rect, 10, 224, b, 2, c);
    o = put(b, "FILE    ");
    for (uint32_t i = 0; nm[i] && i < 22; i++) *o++ = nm[i];
    if (a.found) { o = put(o, "  "); o = put_dec(o, (a.size + 1023u) / 1024u); o = put(o, " KB"); }
    *o = 0;
    pui_text(lm_rect, 10, 244, b, 2, PUI_DIM);
    const struct dl_mod *m = mod_of_file(nm);
    o = put(b, "HOOKS   ");
    o = put_dec(o, a.hooks);
    if (m) {
        o = put(o, "  SETTINGS ");
        o = put_dec(o, lm_knobs(m, 0));
        o = put(o, page_of_file(nm) ? "  PAGE YES" : "  PAGE NO");
    }
    *o = 0;
    pui_text(lm_rect, 10, 264, b, 2, PUI_DIM);
}
/* RESTART TO APPLY?: the mods switched since the boot, over the cards */
static void lm_ask_draw(void)
{
    lm_rect(40, 92, 400, 180, PUI_BG);
    pui_box(lm_rect, 40, 92, 400, 180, 2, PUI_PURPLE);
    pui_fill(lm_rect, 40, 92, 400, 28, PUI_PURPLE);
    const char *q = "RESTART TO APPLY?";
    pui_text(lm_rect, 40 + (400 - pui_width(q, 2)) / 2, 99, q, 2, PUI_BG);
    int y = 132, shown = 0, more = 0;
    for (uint32_t i = 0; i < ram->page_n; i++) {
        int e = boot_err(ram->page[i].name);
        if (ram->page[i].on == (e >= 0)) continue;
        if (shown == 6) { more++; continue; }
        char b[24];
        b[0] = ram->page[i].on ? '+' : '-';
        b[1] = ' ';
        lm_name(b + 2, i);
        pui_text(lm_rect, 60, y, b, 2, ram->page[i].on ? PUI_CYAN : PUI_ORANGE);
        y += 20;
        shown++;
    }
    if (more) {
        char b[16], *o = put(b, "AND ");
        o = put_dec(o, (uint32_t)more);
        o = put(o, " MORE");
        *o = 0;
        pui_text(lm_rect, 60, y, b, 2, PUI_DIM);
    }
}

/* the selected card's icon (or the INFO page's) plays its animation (a frame each LM_ANIM_MS), when nothing else is
   waiting to draw */
static void lm_icon_at(uint32_t at, uint32_t frame)
{
    uint32_t list[MAX_MODS], n = lm_list(list);
    if (at >= n || (ram->lm_mode == LM_CARDS && at / LM_PER != ram->lm_page)) return;
    int slot = (int)(at % LM_PER), k = lm_known_of(ram->page[list[at]].name);
    volatile uint16_t *clip = (volatile uint16_t *)(fw_grid + 0x12EA);
    uint16_t was = *clip;
    *clip = 0;
    if (ram->lm_mode == LM_INFO) pui_hicon(lm_rect, 12, 44, k < 0 ? PUI_H_CUBE : lm_known[k].icon, 2, (int)frame, PUI_BG);
    else pui_hicon(lm_rect, LM_CX(slot % 2) + 12, LM_CY(slot / 2) + 4, k < 0 ? PUI_H_CUBE : lm_known[k].icon, 2, (int)frame, PUI_BG);
    *clip = was;
}
static void lm_animate(void)
{
    uint32_t now = fw_millis;
    if (ram->lm_mode == LM_ASK || now - ram->lm_anim_at < LM_ANIM_MS) return;
    ram->lm_anim_at = now;
    ram->lm_anim = (ram->lm_anim + 1) % PUI_HFRAMES;
    lm_icon_at(ram->lm_sel, ram->lm_anim);
}
static void lm_draw(void)
{
    if (ram->page_dirty) {                          /* entered, or a new page: blank the grid once */
        ram->page_dirty = 0;
        for (int r = 0; r < 23; r++) row(r, "");
        ram->lm_wait = 2;                           /* draw after the grid's own repaint */
        ram->lm_dirty = LM_D_ALL | (ram->lm_mode == LM_ASK ? LM_D_ASK : 0u);
        return;
    }
    if (ram->lm_wait && --ram->lm_wait) return;
    if (!ram->lm_dirty) { lm_animate(); return; }
    volatile uint16_t *clip = (volatile uint16_t *)(fw_grid + 0x12EA);
    uint16_t clip_was = *clip;
    *clip = 0;
    if ((ram->lm_dirty & LM_D_ALL) == LM_D_ALL) lm_rect(0, 30, FW_SCREEN_W, 290, PUI_BG);
    if (ram->lm_mode == LM_INFO && (ram->lm_dirty & ~(LM_D_HEAD | LM_D_FOOT | LM_D_ASK)))
        ram->lm_dirty = (ram->lm_dirty & (LM_D_HEAD | LM_D_FOOT)) | LM_D_BODY;   /* the page instead of the cards */
    uint32_t list[MAX_MODS], n = lm_list(list), page = ram->lm_sel / LM_PER;
    for (uint32_t bit = 0; bit < 12; bit++) {
        uint32_t m = 1u << bit;
        if (!(ram->lm_dirty & m)) continue;
        if (m == LM_D_ASK && (ram->lm_dirty & LM_D_ALL)) continue;   /* the box goes over the cards, last */
        ram->lm_dirty &= ~m;
        if (m == LM_D_HEAD) lm_head();
        else if (m == LM_D_TABS) ram->lm_mode == LM_INFO ? lm_info_draw() : lm_tabs();
        else if (m == LM_D_FOOT) lm_foot();
        else if (m == LM_D_ASK) lm_ask_draw();
        else {
            int slot = (int)bit - 3;
            uint32_t at = page * LM_PER + (uint32_t)slot;
            lm_card(slot, at < n ? (int)list[at] : -1);
            if (at < n) lm_card_frame(slot, at == ram->lm_sel);
            if (!n && !slot) pui_text(lm_rect, (FW_SCREEN_W - pui_width("NO MODS HERE", 2)) / 2, 150, "NO MODS HERE", 2, PUI_DIM);
        }
        if (bit) break;                             /* one piece a frame (the header goes with the next) */
    }
    ram->lm_page = page;
    *clip = clip_was;
}
static void lm_clear(void)                          /* hand the screen back clean (the clip lifted) */
{
    volatile uint16_t *clip = (volatile uint16_t *)(fw_grid + 0x12EA);
    uint16_t was = *clip;
    *clip = 0;
    lm_rect(0, 0, FW_SCREEN_W, 320, PUI_BG);
    *clip = was;
}
static void lm_show(void)
{
    if (!page_installed()) return;
    uint32_t here = fw_ui[FW_UI_VIEW];
    ram->lm_prev = here == FW_VIEW_FREE ? ram->page_mode : 0xFFu;
    ram->lm_back = here;
    if (FW_SD_MOUNTED()) page_read();
    if (ram->lm_tab >= LM_TABS) ram->lm_tab = LM_ALL;
    if (ram->lm_sel >= ram->page_n) ram->lm_sel = 0;
    ram->lm_anim = 0;
    ram->lm_mode = LM_CARDS;
    ram->page_mode = 5;
    ram->page_dirty = 1;
    if (here != FW_VIEW_FREE) fw_set_view(fw_ui, FW_VIEW_FREE);
    lm_clear();
}
static void lm_close(void)
{
    lm_clear();
    ram->lm_mode = LM_CARDS;
    if (ram->lm_prev != 0xFFu && ram->lm_prev != 5u) {
        ram->page_mode = ram->lm_prev;
        if (!ram->page_mode && FW_SD_MOUNTED()) page_read();
        ram->page_dirty = 1;
    } else {
        ram->page_mode = 0;
        fw_set_view(fw_ui, (int)(ram->lm_back == FW_VIEW_FREE ? FW_VIEW_SETTINGS : ram->lm_back));
    }
}
/* back out of the MOD LOADER: if the mods on differ from the ones the boot loaded, ask to restart first */
static void lm_leave(void)
{
    if (!ram->skipped && ram->lm_mode != LM_ASK && page_changed()) {
        ram->lm_mode = LM_ASK;
        ram->lm_dirty |= LM_D_ASK | LM_D_FOOT;
        return;
    }
    lm_close();
}
static void lm_mode_set(uint32_t mode)              /* the cards <-> a mod's INFO page */
{
    ram->lm_mode = mode;
    ram->lm_anim = 0;
    ram->lm_anim_at = fw_millis;
    lm_rect(4, 40, 472, 246, PUI_BG);
    ram->lm_dirty = mode == LM_INFO ? (LM_D_BODY | LM_D_FOOT) : (LM_D_ALL & ~LM_D_HEAD);
}
/* EDIT: the mod's own screen (the visualizer, or its page), else its settings, else its INFO page; SHIFT+RIGHT:
   its settings, else INFO */
static void lm_open(uint32_t file, int settings_only)
{
    const char *nm = ram->page[file].name;
    const struct dl_mod *m = mod_of_file(nm);
    if (!settings_only && lm_opens(file)) {         /* its screen: the visualizer, once the MOD LOADER is gone */
        lm_close();
        ram->lm_vis = 1;
        return;
    }
    const struct dl_page *pg = settings_only ? 0 : page_of_file(nm);
    if (pg) { lm_clear(); api_page_open(pg); return; }   /* its page; OPT there comes back here */
    uint32_t first = 0;
    if (lm_knobs(m, &first)) { ram->mp_page = first; mp_show(); return; }
    if (ram->lm_mode != LM_INFO) lm_mode_set(LM_INFO);
}
static void lm_input(int action)
{
    uint32_t list[MAX_MODS], n = lm_list(list), sel = ram->lm_sel, was = sel;
    if (ram->lm_mode == LM_ASK) {                   /* RESTART TO APPLY? */
        if (action == FW_KEY_EDIT) restart_now();
        if (action == FW_KEY_OPT) lm_close();
        return;
    }
    if (ram->skipped && action == FW_KEY_PLAY) {
        guard_recover();
        ram->lm_dirty |= LM_D_FOOT;
        return;
    }
    if (action == FW_KEY_OPT && ram->lm_mode == LM_INFO) { lm_mode_set(LM_CARDS); return; }
    if (action == FW_KEY_OPT) { lm_leave(); return; }
    if (ram->lm_mode == LM_CARDS && (action == (FW_KEY_SHIFT | FW_KEY_UP) || action == (FW_KEY_SHIFT | FW_KEY_DOWN))) {
        ram->lm_tab = (ram->lm_tab + (action & FW_KEY_DOWN ? 1 : LM_TABS - 1)) % LM_TABS;
        ram->lm_sel = 0;
        ram->lm_anim = 0;
        ram->lm_dirty = LM_D_ALL & ~LM_D_HEAD;
        lm_rect(4, 70, 472, 216, PUI_BG);
        return;
    }
    if (!n) return;
    if (sel >= n) sel = 0;
    uint32_t file = list[sel];
    if (action == FW_KEY_EDIT) { lm_open(file, 0); return; }
    if (action == (FW_KEY_SHIFT | FW_KEY_RIGHT)) { lm_open(file, 1); return; }
    if (action == (FW_KEY_SHIFT | FW_KEY_EDIT)) {   /* on / off from the next RESTART */
        page_set(file, !ram->page[file].on);
        ram->lm_dirty |= LM_D_HEAD | LM_D_FOOT | (ram->lm_mode == LM_INFO ? LM_D_BODY : LM_D_CARD(sel % LM_PER));
        return;
    }
    if (action == (FW_KEY_SHIFT | FW_KEY_LEFT)) {
        lm_mode_set(ram->lm_mode == LM_INFO ? LM_CARDS : LM_INFO);
        return;
    }
    if (ram->lm_mode != LM_CARDS) return;
    if (action == FW_KEY_LEFT && sel % 2) sel--;
    else if (action == FW_KEY_RIGHT && !(sel % 2) && sel + 1 < n) sel++;
    else if (action == FW_KEY_UP && sel >= 2) sel -= 2;
    else if (action == FW_KEY_DOWN && sel + 2 < n) sel += 2;
    else if (action == FW_KEY_DOWN && sel + 1 < n) sel = n - 1;
    if (sel == was) return;
    if (ram->lm_anim) lm_icon_at(was, 0);           /* the icon it leaves stops where it rests */
    ram->lm_anim = 0;
    ram->lm_anim_at = fw_millis;
    ram->lm_sel = sel;
    if (sel / LM_PER != was / LM_PER) {             /* another page of cards */
        lm_rect(4, 70, 472, 216, PUI_BG);
        ram->lm_dirty |= 0x7F8u;
    } else {
        lm_card_frame((int)(was % LM_PER), 0);
        lm_card_frame((int)(sel % LM_PER), 1);
    }
    if (ram->page[list[sel]].on != ram->lm_foot_open) ram->lm_dirty |= LM_D_FOOT;   /* ON / OFF */
}
#endif

static int api_song_data(const struct dl_song_data *d)
{
    if (!d || !d->id || ram->ndata >= MAX_DATA) return 0;
    ram->data[ram->ndata++] = d;
    return 1;
}
static int api_file_read(const char *path, uint32_t off, void *buf, uint32_t n)
{
    struct fw_file f;
    if (!f_open(&f, path, FW_O_READ)) return -1;
    int k = (off && !f_seek(&f, off)) ? -1 : f_read(&f, buf, n);
    fw_file_close(&f);
    return k;
}
#endif

static int api_file_write(const char *path, const void *buf, uint32_t n)
{
    return write_file(path, buf, n) ? (int)n : -1;
}

#ifndef CORE_SD_ONLY
static void api_midi_receive(uint32_t packet)
{
    if (ram->midi_head - ram->midi_tail < MIDI_Q) ram->midi_q[ram->midi_head++ % MIDI_Q] = packet;
}
static void api_midi_send(uint32_t packet) { fw_usb_midi_write_packed(packet); }

/* core_mod_dests: each engine's destination getters (vtable +0xF8 name, +0x104 count, in RAM) are swapped for
   these the first time a mod adds a destination to that engine; `this`'s vtable says which engine it is. */
static const uint32_t ENGINE_VT[FW_ENGINES] = FW_ENGINE_VTABLES;
static int engine_of(const void *eng)
{
    uint32_t vt = *(const uint32_t *)eng;
    for (int i = 0; i < FW_ENGINES; i++) if (ENGINE_VT[i] == vt) return i;
    return -1;
}
static uint32_t core_dest_count(void *eng)
{
    int i = engine_of(eng);
    return i < 0 ? 1 : ram->dest_stock[i] + ram->dest_n[i];
}
static const char *core_dest_name(void *eng, uint32_t dest)
{
    int i = engine_of(eng);
    if (i < 0) return "";
    uint32_t n = ram->dest_stock[i] + ram->dest_n[i];
    if (dest == DEST_MISSING) return "MISSING";
    if (dest >= n) dest = n - 1;
    if (dest < ram->dest_stock[i])                  /* the stock getter clamps with the count: below it, fine */
        return ((const char *(*)(void *, uint32_t))ram->dest_name_orig[i])(eng, dest);
    return ram->dest_extra[i][dest - ram->dest_stock[i]];
}
#if DEST_EXT
static void dest_ext_on(void);
#endif
static int api_mod_dest_add(int engine, const char *name)
{
    if (engine < 0 || engine >= FW_ENGINES || !name) return 0;
    volatile uint32_t *vt = (volatile uint32_t *)ENGINE_VT[engine];
    if (vt[FW_VT_DEST_COUNT / 4] != ((uint32_t)core_dest_count | 1)) {
        ram->dest_name_orig[engine] = vt[FW_VT_DEST_NAME / 4];
        ram->dest_count_orig[engine] = vt[FW_VT_DEST_COUNT / 4];
        uint32_t as_engine = ENGINE_VT[engine];     /* an object whose vtable is this engine's: the getter is
                                                       `movs r0, #N` and reads nothing, but give it the right one */
        ram->dest_stock[engine] = ((uint32_t (*)(void *))ram->dest_count_orig[engine])(&as_engine);
        ram->dest_n[engine] = 0;
        vt[FW_VT_DEST_NAME / 4] = (uint32_t)core_dest_name | 1;
        vt[FW_VT_DEST_COUNT / 4] = (uint32_t)core_dest_count | 1;
    }
    if (ram->dest_n[engine] >= DEST_EXTRA_MAX
        || (!DEST_EXT && ram->dest_stock[engine] + ram->dest_n[engine] >= FW_MOD_DESTS_MAX)) return 0;
    ram->dest_extra[engine][ram->dest_n[engine]++] = name;
    int index = (int)(ram->dest_stock[engine] + ram->dest_n[engine] - 1);
#if DEST_EXT
    dest_ext_on();
#endif
    return index;
}

#if DEST_EXT
/* ---- core 0.6: extended destinations (docs/MOD-DESTS.md) -------------------------------------------------
 * An instrument keeps each modulator's destination in the low 4 bits of a packed byte (type << 4 | dest), which
 * songs and .m8i files store as they are. The stock destinations stay there. A destination a mod added holds
 * DEST_MARK in the byte, and its index is in ram->dests.ext[instrument][modulator]. Core keeps that with the song
 * (song data "core:dests") and with the instrument (clone, paste, clear, .m8i save / load: a <file>.dldest beside
 * it) and a copied modulator. Indices depend on which mods add destinations, and in what order, so the song data
 * and the sidecar also carry each destination's name (hashed): a load maps them to this boot's indices, and a
 * name no mod adds now reads as DEST_MISSING ("MISSING", applied by nobody). The firmware's 4-bit places get
 * core's C instead (bindings "mids"): the INST. MODS field's value, the editor's store, the DEST name drawn,
 * modulator copy / paste, and the randomizer's count (it picks stock destinations only). Mods get the index with
 * api->mod_dest_of. Without core 0.6 such a modulator shows the engine's last destination and does nothing. */
static void dests_loaded(uint32_t n);
static const struct dl_song_data dests_data = { "core:dests", &ram->dests, sizeof ram->dests, dests_loaded };
static void dest_ext_on(void)
{
    for (uint32_t i = 0; i < ram->ndata; i++) if (ram->data[i] == &dests_data) return;
    api_song_data(&dests_data);
}
static uint32_t name_hash(const char *s)            /* FNV-1a, folded to 16 bits; never 0 */
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    h = (h ^ (h >> 16)) & 0xFFFFu;
    return h ? h : 1;
}
static int engine_of_type(uint32_t type)            /* an instrument's type byte -> FW_ENGINE_* */
{
    static const int8_t map[7] = { FW_ENGINE_WAV, FW_ENGINE_MACRO, FW_ENGINE_SAMPLER, FW_ENGINE_MIDI_OUT,
                                   FW_ENGINE_FM, FW_ENGINE_HYPER, FW_ENGINE_EXTERNAL };
    return type < 7 ? map[type] : -1;
}
static int inst_engine(uint32_t inst) { return inst < 128 ? engine_of_type(FW_INSTRUMENT(inst)[0]) : -1; }
/* is `value` one of the destinations a mod added to this instrument's engine? */
static int added_dest(uint32_t inst, uint32_t value)
{
    int e = inst_engine(inst);
    if (e < 0 || !ram->dest_n[e]) return value >= DEST_MARK;
    return value >= ram->dest_stock[e];
}
static uint32_t dest_hash_of(int e, uint32_t index)  /* the name hash of an added destination, 0: none */
{
    if (e < 0 || index == DEST_MISSING || index < ram->dest_stock[e]) return 0;
    uint32_t k = index - ram->dest_stock[e];
    return k < ram->dest_n[e] ? name_hash(ram->dest_extra[e][k]) : 0;
}
static uint32_t dest_by_hash(int e, uint32_t h)      /* this boot's index for a hashed name, DEST_MISSING: none */
{
    if (e < 0 || !h) return DEST_MISSING;
    for (uint32_t k = 0; k < ram->dest_n[e]; k++)
        if (name_hash(ram->dest_extra[e][k]) == h) return ram->dest_stock[e] + k;
    return DEST_MISSING;
}
static void dests_prepare(void)                     /* before the song data is written */
{
    zero(ram->dests.hash, sizeof ram->dests.hash);
    for (int e = 0; e < FW_ENGINES; e++)
        for (uint32_t k = 0; k < ram->dest_n[e] && k < DEST_EXTRA_MAX; k++)
            ram->dests.hash[e][k] = (uint16_t)name_hash(ram->dest_extra[e][k]);
}
/* after a song load: the saved indices, by name, to this boot's. The saved index's place in its engine's added
   list is index - the engine's stock count (15, or 14 for SAMPLER and EXTERNAL; the same every boot). */
static uint32_t stock_count(int e)
{
    return e == FW_ENGINE_SAMPLER || e == FW_ENGINE_EXTERNAL ? 14u : 15u;
}
static void dests_loaded(uint32_t n)
{
    if (n != sizeof ram->dests) { zero(&ram->dests, sizeof ram->dests); return; }
    for (uint32_t i = 0; i < 128; i++) {
        int e = inst_engine(i);
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t v = ram->dests.ext[i][k];
            if (!v || v == DEST_MISSING) continue;
            uint32_t slot = e < 0 || v < stock_count(e) ? DEST_EXTRA_MAX : v - stock_count(e);
            uint32_t h = slot < DEST_EXTRA_MAX ? ram->dests.hash[e][slot] : 0;
            ram->dests.ext[i][k] = (uint8_t)(h ? dest_by_hash(e, h) : DEST_MISSING);
        }
    }
}
static uint32_t dest_real(uint32_t inst, uint32_t k, uint32_t byte)
{
    uint32_t d = byte & 15u;
    if (d == DEST_MARK && inst < 128 && k < 4 && ram->dests.ext[inst][k]) return ram->dests.ext[inst][k];
    return d;
}
/* INST. MODS loads the field under the cursor: (view, modulator x 6, packed byte) -> the value to edit */
uint32_t core_dest_cell(const uint8_t *view, uint32_t k6, uint32_t byte) { return dest_real(view[8], k6 / 6, byte); }
/* the editor stores the clamped value: (view, modulator, value) -> the 4 bits for the packed byte */
uint32_t core_dest_store(const uint8_t *view, uint32_t k, uint32_t value)
{
    uint32_t inst = view[8];
    int ext = inst < 128 && k < 4 && added_dest(inst, value);
    if (inst < 128 && k < 4) ram->dests.ext[inst][k] = (uint8_t)(ext ? value : 0);
    return ext ? DEST_MARK : value & 15u;
}
/* the DEST name drawn: (the screen's model, modulator, packed byte) -> the index name() gets */
uint32_t core_dest_shown(const uint8_t *model, uint32_t k, uint32_t byte) { return dest_real(model[0xC], k, byte); }
/* the randomizer's dest = rand() % count: only the stock destinations, which the byte holds as they are */
uint32_t core_dest_rand_count(void *eng, uint32_t count_fn)
{
    uint32_t c = ((uint32_t (*)(void *))count_fn)(eng);
    int e = engine_of(eng);
    if (e >= 0 && ram->dest_n[e] && c > ram->dest_stock[e]) c = ram->dest_stock[e];
    return c > DEST_MARK ? DEST_MARK : c;
}
/* a modulator copied (SHIFT+OPT) and pasted (SHIFT+EDIT) on INST. MODS: the clipboard is the packed bytes */
void core_dest_copied(const uint8_t *view, uint32_t k)
{
    uint32_t inst = view[8];
    ram->dest_clip = inst < 128 && k < 4 && (FW_INSTRUMENT(inst)[FW_INST_MODS + 6 * k] & 15u) == DEST_MARK
                     ? ram->dests.ext[inst][k] : 0;
}
void core_dest_pasted(const uint8_t *view, uint32_t k6)
{
    uint32_t inst = view[8], k = k6 / 6;
    if (inst < 128 && k < 4) ram->dests.ext[inst][k] = ram->dest_clip;
}
/* whole instruments: ev_inst_copy (clone src -> dst), ev_inst_paste (the copy SHIFT+OPT made: its slot is at
   instview + 0x10, the target at +0xC), ev_inst_clear, ev_inst_save / ev_inst_load (the .dldest sidecar) */
static void dest_move(uint32_t src, uint32_t dst)
{
    if (src < 128 && dst < 128 && src != dst) copy(ram->dests.ext[dst], ram->dests.ext[src], 4);
}
int core_inst_copy(void *instview, int src, int dst) { (void)instview; dest_move((uint32_t)src, (uint32_t)dst); return 0; }
int core_inst_paste(const uint8_t *instview, int action)
{
    (void)action;
    if (instview[0x10] != 0xFF) dest_move(instview[0x10], instview[0xC]);
    return 0;
}
int core_inst_clear(void *song, int slot, int full)
{
    (void)song; (void)full;
    if (slot >= 0 && slot < 128) zero(ram->dests.ext[slot], 4);
    return 0;
}
#define DEST_FILE_MAGIC 0x32444C44u        /* "DLD2": then 4 name hashes, one per modulator (0: stock) */
static char *dest_file(char *out, const char *path)
{
    char *o = out;
    for (uint32_t i = 0; path[i] && i < 120; i++) *o++ = path[i];
    o = put(o, ".dldest");
    *o = 0;
    return out;
}
int core_inst_save(void *songs, const char *path, int slot)
{
    (void)songs;
    if (slot < 0 || slot > 127 || !path) return 0;
    uint32_t w[3] = { DEST_FILE_MAGIC, 0, 0 };
    uint16_t h[4];
    int e = inst_engine((uint32_t)slot), any = 0;
    for (uint32_t k = 0; k < 4; k++) any |= (h[k] = (uint16_t)dest_hash_of(e, ram->dests.ext[slot][k])) != 0;
    if (!any) return 0;                         /* nothing added: no sidecar (a stale one is ignored on load) */
    copy(&w[1], h, sizeof h);
    char p[136];
    write_file(dest_file(p, path), (const char *)w, sizeof w);
    return 0;
}
int core_inst_load(void *songs, const char *path, int slot)
{
    (void)songs;
    if (slot < 0 || slot > 127 || !path) return 0;
    uint32_t w[3] = { 0, 0, 0 };
    char p[136];
    if (api_file_read(dest_file(p, path), 0, w, sizeof w) != (int)sizeof w || w[0] != DEST_FILE_MAGIC) w[1] = w[2] = 0;
    copy(ram->dest_load, &w[1], sizeof ram->dest_load);
    ram->dest_load_slot = (uint8_t)slot;
    ram->dest_load_ok = 1;                      /* the instrument (its type) is in by the next tick */
    return 0;
}
static void dest_load_done(void)
{
    uint32_t slot = ram->dest_load_slot;
    ram->dest_load_ok = 0;
    if (slot > 127) return;
    int e = inst_engine(slot);
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t marked = (FW_INSTRUMENT(slot)[FW_INST_MODS + 6 * k] & 15u) == DEST_MARK;
        ram->dests.ext[slot][k] = (uint8_t)(marked && ram->dest_load[k] ? dest_by_hash(e, ram->dest_load[k]) : 0);
    }
}
/* for mods' ev_mod_apply (audio thread): the real destination of a modulator whose dest() is DEST_MARK */
static int api_mod_dest_of(void *modulator, int dest)
{
    if (dest != (int)DEST_MARK || !modulator) return dest;
    uint8_t *m = (uint8_t *)modulator;
    uint8_t *first = *(uint8_t **)(m + FW_MODULATOR_FIRST);
    if (!first || m < first) return dest;
    const uint8_t *mods = FW_MODULATOR_INST_MODS(first);
    uint32_t k = (uint32_t)(m - first) / FW_MODULATOR_SIZE;
    uint32_t inst = (uint32_t)(mods - FW_INSTRUMENT(0) - FW_INST_MODS) / FW_INST_SIZE;
    if (!mods || mods < FW_INSTRUMENT(0) || inst > 127 || k > 3 || !ram->dests.ext[inst][k]) return dest;
    return ram->dests.ext[inst][k];
}
#else
static int api_mod_dest_of(void *modulator, int dest) { (void)modulator; return dest; }
#endif
static int api_inst_load(const char *path, int slot)
{
    if (slot < 0 || slot > 0x7F || !FW_SD_MOUNTED()) return 0;
    return fw_inst_load(fw_songs, path, slot, 0);
}
static int api_inst_save(const char *path, int slot)
{
    if (slot < 0 || slot > 0x7F || !FW_SD_MOUNTED()) return 0;
    return fw_inst_save(fw_songs, path, slot);
}

static void *api_ram_claim(uint32_t bytes)
{
#ifdef FW_SPARE_RAM
    uint32_t at = (ram->spare_next + 7u) & ~7u;
    if (at < FW_SPARE_RAM || bytes > FW_SPARE_RAM_END - at) return 0;
    ram->spare_next = at + bytes;
    return (void *)at;
#else
    (void)bytes;
    return 0;
#endif
}

static const struct dl_core api = {
    DL_CORE_MAGIC, DL_CORE_VERSION, sizeof(struct dl_core),
    api_restart, api_page_add, api_page_open, api_page_close, api_song_data, api_file_read, api_file_write,
    api_midi_receive, api_midi_send, ram->crash, ram->song, api_inst_load, api_inst_save, api_mod_dest_add,
    api_setting_add, api_menu_open, api_mod_dest_of, api_ram_claim,
};

/* ev_crash: core's handler in front of Teensyduino's for HardFault, MemManage, BusFault and UsageFault (RAM vector
   table). It runs the mods' handlers with the exception frame (r0-r3, r12, lr, pc, xpsr), counts that they ran in
   ITCM (which survives the reset), and goes on to Teensyduino's handler with the stack and lr as they were: that
   handler records the crash and resets. A handler that faults again is skipped (in_fault). Handlers run in the
   fault: no SD, no USB, no waiting. */
#define EV_CRASH_W 0x0004u
uint32_t core_crash(const uint32_t *frame)
{
    if (!ram->in_fault && ram->magic == RAM_MAGIC) {
        ram->in_fault = 1;
        ram->fault_pc = frame[6];
        fire(EV_CRASH_W, (uint32_t)frame, 0);
        ram->fault_handled++;
    }
    return ram->fault_orig;
}
__attribute__((naked)) static void core_fault(void)
{
    __asm volatile(
        "tst    lr, #4          \n"
        "ite    eq              \n"
        "mrseq  r0, msp         \n"
        "mrsne  r0, psp         \n"
        "push   {r4, lr}        \n"
        "bl     core_crash      \n"
        "pop    {r4, lr}        \n"
        "bx     r0              \n");
}

#define BOOT_API (&api)
#endif

static void itcm_writable(void)
{
    volatile uint32_t *rnr = (uint32_t *)0xE000ED98u, *rasr = (uint32_t *)0xE000EDA0u;
    *rnr = 1;                                       /* MPU region 1 = ITCM: read-only -> read/write */
    *rasr = (*rasr & ~(7u << 24)) | (3u << 24);
    __asm volatile("dsb; isb" ::: "memory");
}

/* The ITCM sites' veneers: the builder lists them at CORE_VENEERS (count, then itcm address and flash stub per
   site); each becomes `ldr.w pc, [pc, #0]` and the stub's address, in the ITCM tail above core's RAM. */
#ifndef CORE_VENEERS
#error "CORE_VENEERS comes from the builder"
#endif
static void write_veneers(void)
{
    const uint32_t *t = (const uint32_t *)CORE_VENEERS;
    uint32_t n = t[0];
    if (n > 16) return;                             /* erased flash: no ITCM sites */
    for (uint32_t i = 0; i < n; i++) {
        volatile uint32_t *v = (volatile uint32_t *)t[1 + 2 * i];
        v[0] = 0xF000F8DFu;                         /* ldr.w pc, [pc, #0] */
        v[1] = t[2 + 2 * i];
    }
    __asm volatile("dsb; isb" ::: "memory");
}

void core_early(void)
{
    itcm_writable();
    write_veneers();
    ram->magic = 0;
    ram->loaded = 0;
    ram->api = BOOT_API;
}


/* The skip key at power-on, for a unit without keys: `python -m dirtyloader.unit safe` keeps sending SAFE_MAGIC
   over USB serial while the unit boots. core_load runs about 0.3 s after reset; a computer has configured USB
   within ~10 ms of that, and the tool's first bytes arrive ~160 ms later (measured on the Teensy). So core waits
   up to USB_WAIT_MS for USB, then listens SAFE_MS: a boot costs ~0.35 s more on a computer, 0.25 s on a charger.
   The bytes are all >= 0x80: they start no m8c command (ASCII) and are not core's request mark (0xD1). */
#define USB_WAIT_MS 250u
#define SAFE_MS 350u
static int safe_boot_asked(void)
{
    static const uint8_t want[] = { 0xF5, 0xDA, 0xA5, 0xFE, 0xC4, 0xB7, 0 };
    uint32_t t0 = fw_millis, k = 0;
    while (!fw_usb_configuration && fw_millis - t0 < USB_WAIT_MS) { }
    if (!fw_usb_configuration) return 0;
    t0 = fw_millis;
    while (fw_millis - t0 < SAFE_MS) {
        uint8_t c;
        if (!fw_rx_available || fw_serial_read(&c, 1) != 1) continue;
        k = c == want[k] ? k + 1 : c == want[0];
        if (!want[k]) return 1;
    }
    return 0;
}

void core_load(void)
{
    uint16_t orders[SLOTS];
    char *const status = (char *)SCRATCH;
    char *o = status;
    uint32_t had_mods = ram->sig == RAM_SIG ? ram->nmods : 0;   /* from the boot before, if it was a reset */

    ram->sig = RAM_SIG;
    ram->booted = 0;
    ram->song_pending = 0;
    ram->page_mode = 0;
    ram->npages = ram->ndata = ram->nsettings = 0;
    ram->cur_page = 0;
    ram->midi_head = ram->midi_tail = 0;
    ram->guard_recovery = GUARD_PENDING;
    ram->song[0] = 0;
    ram->usb_last = 0xFFu;
    ram->dest_clip = ram->dest_load_ok = 0;
    zero(ram->dest_n, sizeof ram->dest_n);          /* ITCM keeps the last boot's (or another core's) values */
#ifdef FW_SPARE_RAM
    ram->spare_next = FW_SPARE_RAM;
#endif
    zero(ram->dest_stock, sizeof ram->dest_stock);
    ram->loaded = 1;
    ram->nmods = 0;
    ram->nboot = 0;
    ram->next = AREA_START;
#ifdef FW_MOD_FLASH
    ram->flash_next = FW_MOD_FLASH;
#endif
    ram->mp_page = ram->mp_sel = 0;
    flash_hooks(orders);
    if (!FW_SD_MOUNTED())
        return;

    o = put(o, "dirtyloader core 0.7\nmods area ");
    o = put_dec(o, AREA_END - AREA_START);
    o = put(o, " bytes at ");
    o = put_hex(o, AREA_START);
#ifdef FW_MOD_FLASH
    o = put(o, "; flash ");
    o = put_dec(o, (FW_MOD_FLASH_END - FW_MOD_FLASH) >> 10);
    o = put(o, " KB at ");
    o = put_hex(o, FW_MOD_FLASH);
#endif
    o = put(o, "\n");

    int skip = SKIP_NONE;
    uint32_t guard = read_guard();
    int host_skip = safe_boot_asked();
    zero(ram->crash, sizeof ram->crash);
    if (crashed())
        for (uint32_t i = 0; i < 8; i++) ram->crash[i] = fw_fault_info[i];
    if (crashed() && had_mods) {
        o = put(o, "crash guard: the last boot with mods crashed (pc ");
        o = put_hex(o, fw_fault_info[6]);
        o = put(o, "), so no mods were loaded.\n");
        write_file(GUARD_PATH, "SKIP", 4);
        skip = SKIP_CRASH;
    } else if (host_skip) {
        o = put(o, "safe boot: the host asked (python -m dirtyloader.unit safe), so no mods were loaded.\n");
        write_file(GUARD_PATH, "SKIP", 4);
        skip = SKIP_HOST;
    } else if (guard == G_SKIP) {
        o = put(o, "crash guard: guard says SKIP, so no mods were loaded.\n");
        skip = SKIP_GUARD;
    }
    if (crashed() && ram->sig == RAM_SIG && ram->fault_handled) {
        o = put(o, "ev_crash: the mods' handlers ran in the fault (pc ");
        o = put_hex(o, ram->fault_pc);
        o = put(o, ")\n");
    }
    ram->fault_handled = 0;
    if (crashed()) forget_crash();
    ram->skipped = (uint32_t)skip;
    if (skip)
        o = put(o, "Clear SKIP on the mod screen or write GOOD to dirtyloader/guard, then restart.\n");
    else if (guard == G_ARMD)
        o = put(o, "crash guard: legacy ARMD marker ignored.\n");

    if (!skip) {
        struct fw_file f;
        char *const list = (char *)(SCRATCH + STATUS_SIZE);
        int n = 0;
        if (f_open(&f, "/dirtyloader/active.txt", FW_O_READ)) {
            n = f_read(&f, list, LIST_SIZE - 1);
            fw_file_close(&f);
        }
        if (n < 0) n = 0;
        list[n] = 0;
        char *p = list;
        while (*p && o < status + STATUS_SIZE - 200) {
            char *line = p;
            while (*p && *p != '\n' && *p != '\r') p++;
            if (*p) *p++ = 0;
            while (*line == ' ') line++;
            if (!*line || *line == '#') continue;
            const struct dl_mod *m = 0;
            uint32_t at = ram->next;
            int err = load_one(line, orders, &m);
            if (ram->nboot < MAX_MODS) {
                char *d = ram->boot[ram->nboot].name;
                uint32_t i = 0;
                for (; line[i] && i < NAME_MAX - 1; i++) d[i] = line[i];
                d[i] = 0;
                ram->boot[ram->nboot++].err = err;
            }
            o = put(o, line);
            if (err) {
                o = put(o, ": E");
                o = put_dec(o, err);
                o = put(o, " ");
                o = put(o, ERRORS[err]);
            } else {
                o = put(o, ": loaded ");
                o = put(o, m->id);
                o = put(o, " ");
                o = put(o, m->version);
                o = put(o, " at ");
                o = put_hex(o, at);
                o = put(o, ", ");
                o = put_dec(o, ram->next - at);
                o = put(o, " bytes, ");
                if (ram->last_flash) {                  /* its code in flash */
                    o = put(o, "code in flash at ");
                    o = put_hex(o, ram->last_flash);
                    o = put(o, ", ");
                    o = put_dec(o, ram->last_flash_n);
                    o = put(o, ram->last_written ? " bytes (written), " : " bytes, ");
                }
                uint32_t nh = 0;
                for (uint32_t k = 0; k < m->nsubs; k++) nh += m->subs[k].event != EV_INFO;
                o = put_dec(o, nh);
                o = put(o, " hooks");
            }
            o = put(o, "\n");
        }
    }
    /* ev_card_ready: the card is up and the firmware has not used it; mods are loaded, ev_boot comes later. The
       dispatcher does not use ram->table yet (no magic), so core calls the handlers itself. */
    for (uint32_t i = 0; i < ram->count; i++)
        if (ram->table[i].event == 0x0002u && (ram->table[i].flags & FLAG_ENABLED))
            ((void (*)(int))ram->table[i].fn)(1);
    o = put(o, "hooks ");
    o = put_dec(o, ram->count);
    o = put(o, "\n");
    write_file("/dirtyloader/status.txt", status, (uint32_t)(o - status));
#ifdef CORE_GUARD_DIAGNOSTICS
    /* A later SKIP boot must not overwrite the reason the guard tripped. */
    if (skip == SKIP_CRASH || skip == SKIP_HOST) {
        char fault[160];
        char *end = put_dec(put(fault, "reason "), (uint32_t)skip);
        end = put_hex(put(end, "\nprevious_guard "), guard);
        end = put_hex(put(end, "\ncfsr "), ram->crash[2]);
        end = put_hex(put(end, "\nhfsr "), ram->crash[3]);
        end = put_hex(put(end, "\npc "), ram->crash[6]);
        end = put(end, "\n");
        write_file("/dirtyloader/guard-last-skip.txt", fault, (uint32_t)(end - fault));
    }
#endif
}

void core_boot(void)
{
    if (!ram->loaded) {                             /* setup() never reached core_load: flash hooks only */
        uint16_t orders[SLOTS];
        ram->nmods = 0;
        flash_hooks(orders);
    }
#ifndef CORE_SD_ONLY
#if FW_HAS_VIEW_TABLE
    if (!fw_views[FW_VIEW_FREE]) {                  /* the MODS page takes the firmware's free view slot */
        zero(&ram->page_view, sizeof ram->page_view);
        ram->page_view.vt = page_vt;
        fw_views[FW_VIEW_FREE] = &ram->page_view;
    }
#else
    zero(&ram->page_view, sizeof ram->page_view);   /* set_view has no table: core_view switches to it (ev_view) */
    ram->page_view.vt = page_vt;
#endif
    if (FW_VECTORS[FW_AUDIO_VECTOR] != ((uint32_t)core_audio | 1)) {   /* ev_render_in / ev_render_out */
        ram->audio_orig = FW_VECTORS[FW_AUDIO_VECTOR];
        __asm volatile("dsb" ::: "memory");
        FW_VECTORS[FW_AUDIO_VECTOR] = (uint32_t)core_audio | 1;
    }
#endif
#ifdef CORE_SD_ONLY
    /* Leave the Model:01 tap alone unless a loaded mod uses it. */
    if (has_subs(EV_AUDIO_OUT_W & 0xFFFFu))
#endif
    {                                               /* ev_audio_out */
        volatile uint32_t *vt = *(volatile uint32_t **)(FW_AUDIO_TAP_OBJ);
        ram->audio_subs = 0;
        for (uint32_t i = 0; i < ram->count; i++) if (ram->table[i].event == 0x0032u) ram->audio_subs = 1;
        if (vt && vt[0] != ((uint32_t)core_tap | 1)) {
            ram->tap_orig = vt[0];
            vt[0] = (uint32_t)core_tap | 1;
        }
    }
#if !defined(CORE_SD_ONLY) && defined(FW_ATTR_PAL_LIT) && defined(fw_sys)
    xt_boot();                                      /* the extended theme: its palette and its ev_draw handler */
#endif
    ram->track_subs = has_subs(EV_TRACK_AUDIO_W);
#ifndef CORE_SD_ONLY
    ram->fx_subs = has_subs(EV_FX_AUDIO_W & 0xFFFFu);
    ram->params_subs = has_subs(EV_PARAMS_W);
#endif
    if (ram->track_subs) wrap_slot(FW_SLOT_TRACK, &ram->track_orig, (uint32_t)core_track);
#ifndef CORE_SD_ONLY
    if (ram->fx_subs) {
        wrap_slot(FW_SLOT_MFX, &ram->mfx_orig, (uint32_t)core_mfx);
        wrap_slot(FW_SLOT_DELAY, &ram->delay_orig, (uint32_t)core_delay);
        wrap_slot(FW_SLOT_REVERB, &ram->reverb_orig, (uint32_t)core_reverb);
    }
    if (ram->params_subs) wrap_slot(FW_SLOT_CLOCK, &ram->clock_orig, (uint32_t)core_clock);
    if (has_subs(EV_STEP_EDIT_W)) {                 /* bulk phrase edits */
        ram->snap_phrase = 0xFFFFFFFFu;
        wrap_slot(FW_SLOT_PHRASE_BULK, &ram->bulk_orig, (uint32_t)core_phrase_bulk);
        wrap_slot(FW_SLOT_PHRASE_CELL, &ram->cell_orig, (uint32_t)core_phrase_cell);
    }
    if (FW_VECTORS[3] != ((uint32_t)core_fault | 1)) {   /* ev_crash: HardFault .. UsageFault */
        ram->fault_orig = FW_VECTORS[3];
        ram->in_fault = 0;
        for (int v = 3; v <= 6; v++) FW_VECTORS[v] = (uint32_t)core_fault | 1;
    }
    if (fw_sysex_handler != (uint32_t)core_sysex) {   /* ev_sysex */
        ram->sysex_orig = fw_sysex_handler;
        fw_sysex_handler = (uint32_t)core_sysex;
    }
#endif
#ifdef CORE_MODEL01_MENU
    model01_install_menu();
#endif
    ram->booted = 1;                                /* song loads from here on are the user's */
    ram->api = BOOT_API;
    __asm volatile("dsb\n isb" ::: "memory");
    ram->magic = RAM_MAGIC;
}
