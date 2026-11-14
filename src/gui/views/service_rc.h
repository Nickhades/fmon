/*
 * service_rc.h - rc.conf 解析与“开机自启”状态（纯 GLib，不依赖 GTK）
 *
 * 单独成文件是为了让 tests/test_service_rc.c 直接编译测试解析器；
 * rc.d 目录扫描仍在 service_model.c（依赖 GUI 侧的 token 校验）。
 */

#ifndef SERVICE_RC_H
#define SERVICE_RC_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* rc.conf 中 “服务名_enable” 的后缀 */
#define SVC_ENABLE_SUFFIX "_enable"

/* 按 rc(8) 的顺序读取 rc.conf / rc.conf.local / rc.conf.d，
 * 返回 “变量 -> 值” 表（调用者负责 g_hash_table_destroy）。 */
GHashTable *svc_rc_load(void);

/* 是否 *_enable=YES/1/TRUE（大小写不敏感） */
gboolean svc_rc_enabled(GHashTable *vars, const char *name);

/* 解析一段 rc.conf 文本并合并进 vars（后出现的覆盖先出现的）。
 * 供 svc_rc_load() 与单元测试使用。 */
void svc_rc_parse_text(GHashTable *vars, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_RC_H */
