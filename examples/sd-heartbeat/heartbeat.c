/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SD heartbeat writes /dirtyloader/sd-heartbeat.txt once at boot to confirm
 * that core loaded the mod and its BOOT handler ran. Remove the marker
 * before restarting to check for a fresh write.
 *
 * On a bare Teensy 4.1 it also double-blinks the LED on pin 13, changing to
 * three flashes once a tempo change is seen. Both handhelds leave the pin alone.
 *
 *     python -m dirtyloader.cc build examples/sd-heartbeat */
#include "dl/mod.h"
#include "m8-headless/fw.h"
#include "dl/core.h"

#if !defined(M8_MODEL01) && !defined(M8_MODEL02)
#define GPIO7_GDIR   (*(volatile uint32_t *)0x42004004u)
#define GPIO7_SET    (*(volatile uint32_t *)0x42004084u)
#define GPIO7_CLEAR  (*(volatile uint32_t *)0x42004088u)
#define LED          (1u << 3)

static uint32_t tempo_seen;
static const uint8_t pattern[8] = { 1, 0, 1, 0, 0, 0, 0, 0 };

static void led_setup(void)
{
    *(volatile uint32_t *)0x401F8148u = 5;          /* pad GPIO_B0_03 -> GPIO */
    *(volatile uint32_t *)0x401F8338u = 0x38;
    *(volatile uint32_t *)0x400AC06Cu |= LED;       /* GPR27: route it to GPIO7 */
    GPIO7_GDIR |= LED;
}

static void on_tick(void)
{
    uint32_t phase = (fw_millis >> 7) & 7;
    int on = tempo_seen ? (phase < 6 && !(phase & 1)) : pattern[phase];
    if (on) GPIO7_SET = LED; else GPIO7_CLEAR = LED;
}

static int on_tempo(float bpm)
{
    (void)bpm;
    tempo_seen = 1;
    return 0;                                       /* let stock change the tempo */
}
#endif

static void on_boot(void)
{
    static const char result[] = "SD heartbeat: BOOT ran\n";
    dl_core_file_write("/dirtyloader/sd-heartbeat.txt", result, sizeof result - 1);
#if !defined(M8_MODEL01) && !defined(M8_MODEL02)
    led_setup();
#endif
}

DL_MOD("sd-heartbeat", "0.1.3", "SD heartbeat",
       DL_INFO("dirtyloader", "Writes a marker on the SD card at boot. On a bare Teensy it also blinks the LED."),
       DL_ON(EV_BOOT, 50, on_boot)
#if !defined(M8_MODEL01) && !defined(M8_MODEL02)
       , DL_ON(EV_TICK, 50, on_tick), DL_ON(EV_TEMPO, 50, on_tempo)
#endif
);
