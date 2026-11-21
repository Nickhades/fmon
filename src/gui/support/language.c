/*
 * language.c - 界面语言配置与 gettext 环境
 */

#include "language.h"

#include <glib/gstdio.h>
#include <string.h>

static FmonLanguage g_lang = FMON_LANGUAGE_ZH;

static gchar *language_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "fmon", "language", NULL);
}

static const gchar *system_language(void)
{
    const gchar *v = g_getenv("LC_ALL");

    if (v == NULL || *v == '\0')
        v = g_getenv("LC_MESSAGES");
    if (v == NULL || *v == '\0')
        v = g_getenv("LANG");

    return v;
}

static FmonLanguage detect_system_language(void)
{
    const gchar *v = system_language();

    if (v != NULL && g_ascii_strncasecmp(v, "en", 2) == 0)
        return FMON_LANGUAGE_EN;

    return FMON_LANGUAGE_ZH;
}

static void apply_gettext_language(void)
{
    if (g_lang == FMON_LANGUAGE_EN)
        g_setenv("LANGUAGE", "en", TRUE);
    else
        g_setenv("LANGUAGE", "zh_CN:zh", TRUE);
}

void fmon_language_init(void)
{
    gchar *path = language_config_path();
    gchar *data = NULL;

    if (g_file_get_contents(path, &data, NULL, NULL) && data != NULL) {
        g_strstrip(data);

        if (g_strcmp0(data, "en") == 0)
            g_lang = FMON_LANGUAGE_EN;
        else if (g_strcmp0(data, "zh") == 0)
            g_lang = FMON_LANGUAGE_ZH;
        else
            g_lang = detect_system_language();
    } else {
        g_lang = detect_system_language();
    }

    g_free(data);
    g_free(path);

    apply_gettext_language();
}

FmonLanguage fmon_language_get(void)
{
    return g_lang;
}

const char *fmon_language_code(void)
{
    return (g_lang == FMON_LANGUAGE_EN) ? "en" : "zh";
}

const char *fmon_language_label(void)
{
    return (g_lang == FMON_LANGUAGE_EN) ? "English" : "中文";
}

void fmon_language_set(FmonLanguage lang)
{
    if (lang != FMON_LANGUAGE_ZH && lang != FMON_LANGUAGE_EN)
        return;

    g_lang = lang;
    apply_gettext_language();

    gchar *path = language_config_path();
    gchar *dir = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, fmon_language_code(), -1, NULL);

    g_free(dir);
    g_free(path);
}
