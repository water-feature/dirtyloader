#include "m8-model01-6.6.4A/fw.h"

static void clear_file(struct fw_file *f)
{
    volatile uint8_t *p = (void *)f;
    for (unsigned i = 0; i < sizeof *f; i++) p[i] = 0;
}

static int open_file(struct fw_file *f, const char *path, int mode)
{
    return FW_SD_MOUNTED() && fw_file_open(f, fw_sd, path, mode);
}

static int transfer(struct fw_file *f, void *data, uint32_t size, int writing)
{
    if (f->fat) {
        fw_io_fn io = writing ? fw_fat_write : fw_fat_read;
        return io(f->fat, data, size);
    }
    if (f->exfat) {
        fw_io_fn io = writing ? fw_exfat_write : fw_exfat_read;
        return io(f->exfat, data, size);
    }
    return -1;
}

#define MOD_BASE 0x0002B2A0u
#define STATE (*(volatile uint32_t *)0x0002B6A0u)

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return ~c;
}

static void itcm_writable(void)
{
    volatile uint32_t *const rnr = (void *)0xE000ED98u;
    volatile uint32_t *const rasr = (void *)0xE000EDA0u;
    uint32_t previous = *rnr;
    *rnr = 1;                       /* Stock Model:01 startup selects ITCM as region 1. */
    *rasr = (*rasr & ~(7u << 24)) | (3u << 24);
    *rnr = previous;
    __asm volatile("dsb; isb" ::: "memory");
}

void probe_load(void)
{
    itcm_writable();
    STATE = 0;
    struct fw_file f;
    clear_file(&f);
    if (!open_file(&f, "/M01-TICK.m8x", FW_O_READ)) return;
    uint8_t *blob = (void *)MOD_BASE;
    int n = transfer(&f, blob, PROBE_SIZE + 1, 0);
    if (!fw_file_close(&f) || n != PROBE_SIZE || crc32(blob, PROBE_SIZE) != PROBE_CRC) return;

    /* Only the companion mod built with this probe is accepted. Its validated
       DLX2 layout is provided by the host builder. Copy down over the header. */
    for (unsigned i = 0; i < PROBE_IMAGE + 4u * PROBE_RELOCS; i++) blob[i] = blob[i + PROBE_HEADER];
    uint32_t *rel = (void *)(blob + PROBE_IMAGE);
    for (unsigned i = 0; i < PROBE_RELOCS; i++) {
        *(uint32_t *)(blob + rel[i]) += MOD_BASE;
    }
    for (unsigned i = 0; i < PROBE_BSS; i++) blob[PROBE_IMAGE + i] = 0;
    __asm volatile("dsb\n isb" ::: "memory");
    STATE = MOD_BASE + PROBE_HANDLER;
}

void probe_tick(void)
{
    uint32_t handler = STATE;
    if (!handler) return;
    uint32_t (*tick)(void) = (void *)handler;
    if (tick() < 16u) return;
    STATE = 0;
    struct fw_file f;
    clear_file(&f);
    if (!open_file(&f, "/M01-TICK.txt", FW_O_RDWR | FW_O_CREAT | FW_O_TRUNC)) return;
    static const char result[] = "Model:01 SD probe: loaded and ran 16 ticks\n";
    transfer(&f, (void *)result, sizeof result - 1, 1);
    fw_file_close(&f);
}
