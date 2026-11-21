#include "dl/mod.h"
#include "m8-model01-6.6.4A/fw.h"

/* Return the count so the bring-up loader can verify that this handler ran. */
static uint32_t count_tick(void)
{
    static uint32_t count;
    return ++count;
}

DL_MOD("model01-sd-probe", "0.1.0", "Model:01 SD probe", DL_ON(EV_TICK, 0, count_tick));
