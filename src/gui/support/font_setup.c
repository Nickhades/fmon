/*
 * font_setup.c - 自带的字体渲染设置实现
 *
 * 为什么要自己做：
 *   本程序在 root 下启动时不会读取桌面用户的 fontconfig 配置，
 *   系统默认又把 Sans 映射到“文泉驿正黑”这类点阵感较强的字体，
 *   于是文字看起来发虚、有锯齿。这里把渲染参数和字体族直接注入
 *   fontconfig，使输出只取决于本程序的设置，而不是桌面环境。
 */

#include "font_setup.h"

#include <glib.h>
#include <fontconfig/fontconfig.h>
#include <string.h>
#include <unistd.h>

/*
 * 渲染参数说明：
 *   antialias      = true   打开抗锯齿
 *   rgba           = rgb    次像素渲染（LCD 屏，横向 RGB 排列）
 *   hinting        = true   打开字形微调
 *   hintstyle      = hintslight  轻微微调：比 full 更圆润，比 none 更清晰
 *   lcdfilter      = lcddefault  LCD 次像素滤波，进一步减少彩边
 *   embeddedbitmap = false  不使用字体自带的点阵图（点阵会显得有锯齿）
 *   autohint       = false  交给字体自身的 hint 数据
 *
 * 字体族：sans-serif 优先 Noto Sans，其次 Noto Sans CJK SC、DejaVu Sans，
 * 避免落到低质量的默认备用字体。
 */
static const char *const fc_config_xml =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
    "<fontconfig>\n"
    "  <match target=\"font\">\n"
    "    <edit name=\"antialias\" mode=\"assign\"><bool>true</bool></edit>\n"
    "    <edit name=\"rgba\" mode=\"assign\"><const>rgb</const></edit>\n"
    "    <edit name=\"hinting\" mode=\"assign\"><bool>true</bool></edit>\n"
    "    <edit name=\"hintstyle\" mode=\"assign\"><const>hintslight</const></edit>\n"
    "    <edit name=\"lcdfilter\" mode=\"assign\"><const>lcddefault</const></edit>\n"
    "    <edit name=\"autohint\" mode=\"assign\"><bool>false</bool></edit>\n"
    "    <edit name=\"embeddedbitmap\" mode=\"assign\"><bool>false</bool></edit>\n"
    "  </match>\n"
    "  <alias>\n"
    "    <family>sans-serif</family>\n"
    "    <prefer>\n"
    "      <family>Noto Sans</family>\n"
    "      <family>Noto Sans CJK SC</family>\n"
    "      <family>DejaVu Sans</family>\n"
    "    </prefer>\n"
    "  </alias>\n"
    "</fontconfig>\n";

void fmon_font_setup(void)
{
    /* 1. 用 CPU（Cairo）渲染：文字由 FreeType 光栅化，
     *    只受下面注入的 fontconfig 参数影响，与 GPU/桌面合成器无关。 */
    g_setenv("GSK_RENDERER", "cairo", FALSE);

    /* 2. 注入 fontconfig 渲染参数与字体偏好 */
    if (!FcInit())
        return;

    gchar *path = NULL;
    gint   fd   = g_file_open_tmp("fmon-fonts-XXXXXX.conf", &path, NULL);

    if (fd < 0 || path == NULL) {
        g_free(path);
        return;
    }

    size_t   len = strlen(fc_config_xml);
    gboolean ok  = (write(fd, fc_config_xml, len) == (ssize_t)len);

    close(fd);

    if (ok) {
        FcConfig *cfg = FcConfigGetCurrent();

        if (cfg != NULL &&
            FcConfigParseAndLoad(cfg, (const FcChar8 *)path, FcFalse))
            FcConfigSetCurrent(cfg);
    }

    unlink(path);
    g_free(path);
}
