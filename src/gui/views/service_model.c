/*
 * service_model.c - 服务页数据层
 *
 * rc.d 脚本扫描与“第三方服务”判定；rc.conf 解析在 service_rc.c
 * （纯 GLib，可单测）。不创建控件，也不发起子进程
 * （运行状态由 service_view.c 异步查询）。
 */

#include "service_internal.h"

#include <string.h>


/* ===========================================================================
 * 目录扫描
 * ========================================================================= */
/* 注意：g_ptr_array_sort_values() 传给比较函数的是数组元素本身
 * （这里是 char*），而不是指向元素的指针。旧版常用的
 * g_ptr_array_sort() 才是传“指向元素的指针”，两者不能混用。 */
static gint cmp_str(gconstpointer a, gconstpointer b)
{
    return g_strcmp0((const char *)a, (const char *)b);
}

GPtrArray *svc_scan_scripts(void)
{
    static const char *const dirs[] = { "/etc/rc.d",
                                        FMON_LOCALBASE "/etc/rc.d" };

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);

    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        GDir *dir = g_dir_open(dirs[d], 0, NULL);

        if (dir == NULL)
            continue;

        const gchar *name = NULL;

        while ((name = g_dir_read_name(dir)) != NULL) {
            if (name[0] == '.')
                continue;
            /* 非法文件名直接忽略，避免后续任何形式的参数注入 */
            if (!fmon_gui_is_valid_token(name))
                continue;

            gchar   *path    = g_build_filename(dirs[d], name, NULL);
            gboolean regular = g_file_test(path, G_FILE_TEST_IS_REGULAR);
            g_free(path);

            if (!regular)
                continue;

            gboolean dup = FALSE;
            for (guint i = 0; i < names->len; i++) {
                if (strcmp(g_ptr_array_index(names, i), name) == 0) {
                    dup = TRUE;
                    break;
                }
            }
            if (!dup)
                g_ptr_array_add(names, g_strdup(name));
        }

        g_dir_close(dir);
    }

    g_ptr_array_sort_values(names, cmp_str);
    return names;
}

/* 脚本是否声明了 reload 动作（reload_cmd / extra_commands / reload 函数） */
gboolean svc_supports_reload(const char *name)
{
    static const char *const dirs[] = { FMON_LOCALBASE "/etc/rc.d",
                                        "/etc/rc.d" };
    gboolean supported = FALSE;

    for (size_t d = 0; d < G_N_ELEMENTS(dirs) && !supported; d++) {
        gchar *path = g_build_filename(dirs[d], name, NULL);
        gchar *text = NULL;

        if (g_file_get_contents(path, &text, NULL, NULL)) {
            if (strstr(text, "reload_cmd=") != NULL ||
                strstr(text, "reload_cmd =") != NULL ||
                strstr(text, "reload()") != NULL ||
                (strstr(text, "extra_commands") != NULL &&
                 strstr(text, "reload") != NULL))
                supported = TRUE;

            g_free(text);
        }

        g_free(path);
    }

    return supported;
}

gboolean svc_is_third_party(const char *name)
{
    gchar   *path = g_build_filename(FMON_LOCALBASE "/etc/rc.d", name, NULL);
    gboolean ok   = g_file_test(path, G_FILE_TEST_IS_REGULAR);

    g_free(path);
    return ok;
}
