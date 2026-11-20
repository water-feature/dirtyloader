/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dl/mod.h"
#include "m8-model01-6.6.4A/fw.h"

static void tick(void)
{
    static uint32_t count;
    if (count >= 16 || ++count != 16 || !FW_SD_MOUNTED()) return;

    struct fw_file file;
    volatile uint8_t *bytes = (void *)&file;
    for (unsigned i = 0; i < sizeof file; i++) bytes[i] = 0;
    if (!fw_file_open(&file, fw_sd, "/dirtyloader/model01-tick.txt", FW_O_RDWR | FW_O_CREAT | FW_O_TRUNC))
        return;
    static const char result[] = "Model:01 standard core: ran 16 ticks\n";
    if (file.fat) fw_fat_write(file.fat, (void *)result, sizeof result - 1);
    if (file.exfat) fw_exfat_write(file.exfat, (void *)result, sizeof result - 1);
    fw_file_close(&file);
}

DL_MOD("model01-tick", "0.1.0", "Model:01 tick test", DL_ON(EV_TICK, 0, tick));
