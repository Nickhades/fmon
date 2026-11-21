#include "palette.h"

#include "theme.h"

typedef struct {
    double light[3];
    double dark[3];
} PaletteEntry;

/* 终端 ANSI 风格：亮色只用于数据线；浅色主题用同色相暗档保证对比度。 */
static const PaletteEntry k_palette[FMON_COLOR_COUNT] = {
    [FMON_COLOR_CPU]        = { { 0.00, 0.55, 0.00 }, { 0.00, 1.00, 0.00 } },
    [FMON_COLOR_MEM]        = { { 0.00, 0.55, 0.55 }, { 0.00, 1.00, 1.00 } },
    [FMON_COLOR_SWAP]       = { { 0.67, 0.00, 0.67 }, { 1.00, 0.00, 1.00 } },
    [FMON_COLOR_NET_RX]     = { { 0.00, 0.55, 0.55 }, { 0.00, 1.00, 1.00 } },
    [FMON_COLOR_NET_TX]     = { { 0.67, 0.00, 0.67 }, { 1.00, 0.00, 1.00 } },
    [FMON_COLOR_DISK_READ]  = { { 0.67, 0.55, 0.00 }, { 1.00, 1.00, 0.00 } },
    [FMON_COLOR_DISK_WRITE] = { { 0.80, 0.35, 0.00 }, { 1.00, 0.55, 0.00 } },
    [FMON_COLOR_TEMP]       = { { 0.80, 0.00, 0.00 }, { 1.00, 0.30, 0.30 } },
    [FMON_COLOR_GPU]        = { { 0.67, 0.55, 0.00 }, { 1.00, 1.00, 0.00 } },
    [FMON_COLOR_POOL]       = { { 0.80, 0.40, 0.00 }, { 1.00, 0.65, 0.00 } },
    [FMON_COLOR_ARC]        = { { 0.67, 0.00, 0.67 }, { 1.00, 0.00, 1.00 } },
    [FMON_COLOR_HIT]        = { { 0.00, 0.55, 0.00 }, { 0.00, 1.00, 0.00 } },
    [FMON_COLOR_BATTERY]    = { { 0.00, 0.55, 0.00 }, { 0.00, 1.00, 0.00 } },
};

void fmon_palette_get(FmonColorSlot slot, double rgba[4])
{
    if (rgba == NULL)
        return;

    if (slot < 0 || slot >= FMON_COLOR_COUNT)
        slot = FMON_COLOR_CPU;

    const double *c = fmon_theme_is_dark() ? k_palette[slot].dark
                                          : k_palette[slot].light;

    rgba[0] = c[0];
    rgba[1] = c[1];
    rgba[2] = c[2];
    rgba[3] = 1.0;
}
