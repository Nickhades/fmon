/*
 * service_rc.c - rc.conf 解析（取代逐服务调用 sysrc）
 *
 * 只依赖 GLib：便于 tests/test_service_rc.c 做纯函数测试。
 */

#include "service_rc.h"

#include <string.h>

void svc_rc_parse_text(GHashTable *vars, const char *text)
{
    if (vars == NULL || text == NULL)
        return;

    gchar **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strdup(lines[i]);

        g_strstrip(line);
        if (line[0] == '\0' || line[0] == '#') {
            g_free(line);
            continue;
        }

        gchar *eq = strchr(line, '=');
        if (eq == NULL) {
            g_free(line);
            continue;
        }

        *eq = '\0';

        gchar *key = g_strdup(line);
        gchar *val = g_strdup(eq + 1);

        g_strstrip(key);
        g_strstrip(val);

        /* 值处理：
         *   - 引号形式 "YES" / 'YES'：取引号之间的内容，忽略行尾注释；
         *   - 裸值 YES / YES  # 注释：截断到空白或 '#'。 */
        if (val[0] == '"' || val[0] == '\'') {
            gchar quote  = val[0];
            gchar *close = strrchr(val + 1, quote);

            if (close != NULL) {
                *close = '\0';
                gchar *inner = g_strdup(val + 1);
                g_free(val);
                val = inner;
            }
        } else {
            for (gchar *c = val; *c != '\0'; c++) {
                if (*c == '#' || g_ascii_isspace(*c)) {
                    *c = '\0';
                    break;
                }
            }
        }

        if (key[0] != '\0')
            g_hash_table_replace(vars, key, val);
        else
            g_free(val);

        g_free(line);
    }

    g_strfreev(lines);
}

static void parse_rc_conf_file(GHashTable *vars, const char *path)
{
    gchar *content = NULL;

    if (!g_file_get_contents(path, &content, NULL, NULL))
        return;

    svc_rc_parse_text(vars, content);
    g_free(content);
}

static void parse_rc_conf_dir(GHashTable *vars, const char *dirpath)
{
    GDir *dir = g_dir_open(dirpath, 0, NULL);

    if (dir == NULL)
        return;

    GPtrArray   *files = g_ptr_array_new_with_free_func(g_free);
    const gchar *name  = NULL;

    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.')
            continue;
        g_ptr_array_add(files, g_build_filename(dirpath, name, NULL));
    }
    g_dir_close(dir);

    g_ptr_array_sort_values(files, (GCompareFunc)g_strcmp0);

    for (guint i = 0; i < files->len; i++) {
        const char *path = g_ptr_array_index(files, i);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
            parse_rc_conf_file(vars, path);
    }

    g_ptr_array_free(files, TRUE);
}

GHashTable *svc_rc_load(void)
{
    GHashTable *vars =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    /* 顺序与 rc(8) 一致：后面的覆盖前面的 */
    parse_rc_conf_file(vars, "/etc/rc.conf");
    parse_rc_conf_file(vars, "/etc/rc.conf.local");
    parse_rc_conf_dir(vars, "/etc/rc.conf.d");
    parse_rc_conf_dir(vars, FMON_LOCALBASE "/etc/rc.conf.d");

    return vars;
}

gboolean svc_rc_enabled(GHashTable *vars, const char *name)
{
    gchar key[256];

    if (vars == NULL || name == NULL)
        return FALSE;

    g_snprintf(key, sizeof(key), "%s%s", name, SVC_ENABLE_SUFFIX);

    const char *val = g_hash_table_lookup(vars, key);
    if (val == NULL)
        return FALSE;

    return (g_ascii_strcasecmp(val, "YES") == 0 || strcmp(val, "1") == 0 ||
            g_ascii_strcasecmp(val, "TRUE") == 0);
}
