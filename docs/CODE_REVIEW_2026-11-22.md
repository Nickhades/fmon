# 代码审核 / 模块化记录（2026-11-22）

## 1. 结论

- 语言标准：C11（`-std=c11`，不是 gnu11）。
- 编译警告：GUI/daemon/tests 全量 0 warning。
- 单元测试：20/20（新增 log_filter 测试）。
- 模块静态状态：文件内 `static`；跨文件接口只放头文件。
- FreeBSD 数据面：sysctl / kvm / libprocstat / devstat / ioctl；
  不在 GUI 主线程同步执行外部命令，不依赖 Linux `/proc`。
- 特权边界：GUI 不直接以 root 运行；需要特权的操作走 daemon 控制通道
  或 pkexec/doas，普通采集由 root daemon 发布共享内存快照。

## 2. 本轮已拆出的独立模块

| 模块 | 原位置 | 职责 |
|---|---|---|
| `app/gui_window_state.c/.h` | `gui_window.c` | 窗口大小持久化、最大化处理、X11 居中；不依赖页面/导航 |
| `app/gui_language_switch.c/.h` | `gui_window.c` | 语言切换、exec 重启、可执行路径 |
| `views/log_filter.c/.h` | `log_view.c` | syslog 级别、时间范围、正则/关键词、去重键，纯 GLib，有单测 |
| `views/log_sources.c/.h` | `log_view.c` | 系统/包/Jail 日志源发现；不依赖 GTK |
| `views/log_render.c/.h` | `log_view.c` | 日志过滤、PID 标记、上下文提取 |
| `views/log_context.c/.h` | `log_view.c` | 上下文窗口与 PID 跳转 |
| `views/log_tail.c/.h` | `log_view.c` | 增量读取、文件监听 |
| `views/service_detail.c` | `service_actions.c` | 服务详情、依赖树、状态/日志文本窗口（接口在 service_internal.h） |
| `views/service_batch.c/.h` | `service_actions.c` | 批量服务启停 |
| `pages/gui_cpu_detail.c/.h` | `gui_page_cpu.c` | CPU 时间分布、硬件型号、调度状态 |
| `pages/gui_cpu_gauge.c/.h` | `gui_page_cpu.c` | CPU 使用率仪表与健康摘要 |
| `pages/cpu_curve_util.c/.h` | `gui_page_cpu.c` | CPU 曲线窗口平均/最大值纯函数 |
| `pages/cpu_chart_util.c/.h` | `gui_page_cpu.c` | CPU 图坐标轴/时间轴 |
| `pages/gui_cpu_temp_chart.c/.h` | `gui_page_cpu.c` | CPU 温度曲线与温度计 |
| `views/network_columns.c` + `network_internal.h` | `network_view.c` | 网络条目 GObject、列工厂、排序 |
| `views/process_tree.c/.h` | `process_view.c` | 进程树排序、树形模式切换 |
| `views/process_export.c/.h` | `process_view.c` | 进程列表 CSV 导出 |
| `views/process_hint.c/.h` | `process_view.c` | 进程页提示行 |
| `views/process_scroll.c/.h` | `process_view.c` | 列表滚回顶部 |
| `views/process_selection.c/.h` | `process_view.c` | 选中保持、滚动恢复、操作可用性 |
| `views/process_menu.c/.h` | `process_actions.c` | 右键菜单、行激活 |
| `views/process_menu_ops.c/.h` | `process_actions.c` | 进程树/批量结束菜单操作 |
| `views/history_data.c` + `history_internal.h` | `history_view.c` | 历史 TSV 读取/降采样、刷新 |
| `views/history_export.c` | `history_view.c` | 历史 CSV 导出 |
| `history/history_parse.c` | `history_provider.c` | history TSV 行解析、分层读取、分桶 |
| `charts/gui_chart_interact.c` | `gui_chart.c` | 滚轮缩放、框选、右键平移 |
| `charts/gui_chart_view.c` | `gui_chart.c` | 时间窗口、视图范围、框选状态 |
| `charts/gui_chart_config.c` | `gui_chart.c` | 自动量程配置 |
| `charts/gui_chart_draw.c` | `gui_chart.c` | 坐标、阈值、统计、边框 |
| `collectors/collector_gpu_nvidia.c` | `collector_gpu.c` | NVIDIA NVML/nvidia-smi 采集 |
| `collectors/collector_gpu_probe.c/.h` | `collector_gpu.c` | PCI 枚举、型号表、二进制解析 |
| `core/fmon_ctl_service.c` + `fmon_ctl_internal.h` | `fmon_ctl.c` | SERVICE / SYSRC 控制命令 |
| `core/fmon_ctl_proc.c` | `fmon_ctl.c` | SIGNAL / RENICE / CPUSET 控制命令 |
| `charts/chart_geom_polyline.c` | `chart_geom.c` | 采样抽取、折线点生成 |
| `charts/chart_geom_cairo.c` | `chart_geom.c` | Cairo 路径/边界带 |
| `core/history_sidecars.c/.h` | `history_store.c` | GPU/ZFS 1 分钟 sidecar 历史 |
| `charts/combo_chart_hover.c` | `combo_chart_draw.c` | 组合图悬停十字线/读数框 |
| `collectors/collector_net_detail.c/.h` | `collector_net.c` | ifconfig/WiFi 接口明细 |
| `pages/gui_page_gpu_detail.c` | `gui_page_gpu.c` | GPU 详情卡片 |
| `collectors/collector_io_meta.c/.h` | `collector_io.c` | 磁盘型号/CAM/USB/容量元数据缓存 |
| `views/storage_columns.c` + `storage_internal.h` | `storage_view.c` | 存储条目列、单元格工厂、使用率条 |
| `support/disk_history_store.c` | `disk_history.c` | 磁盘历史采样、SQLite 写入/查询、清理 |
| `support/disk_history_draw.c` | `disk_history.c` | 磁盘历史曲线/悬停绘制 |
| `pages/gui_zfs_zpool.c/.h` | `gui_page_zfs.c` | ZFS 池摘要、详情、vdev、错误趋势 |
| `pages/gui_zfs_arc.c/.h` | `gui_page_zfs.c` | ARC 明细与构成趋势 |
| `pages/zpool_parse.c/.h` | `zpool_health.c` | zpool 输出纯解析（status/iostat/vdev/latency/txg） |
| `pages/gui_page_prune.c/.h` | `gui_page_zfs.c` | ARC Prune 明细、回收状态、MRU/MFU ghost 曲线 |

拆分后：

| 文件 | 行数 |
|---|---|
| `gui_window.c` | 694 |
| `gui_language_switch.c` | 98 |
| `gui_window_state.c` | 212 |
| `log_view.c` | 1145 |
| `log_filter.c` | 171 |
| `log_sources.c` | 139 |
| `log_view.c` | 445 |
| `log_render.c` | 254 |
| `log_context.c` | 247 |
| `log_tail.c` | 223 |
| `service_actions.c` | 497 |
| `service_batch.c` | 275 |
| `service_detail.c` | 586 |
| `gui_page_cpu.c` | 385 |
| `gui_cpu_detail.c` | 374 |
| `gui_cpu_gauge.c` | 346 |
| `gui_cpu_temp_chart.c` | 280 |
| `cpu_chart_util.c` | 91 |
| `cpu_curve_util.c` | 103 |
| `gui_page_zfs.c` | 441 |
| `gui_zfs_zpool.c` | 447 |
| `gui_zfs_arc.c` | 201 |
| `zpool_health.c` | 368 |
| `zpool_parse.c` | 505 |
| `network_view.c` | 563 |
| `network_columns.c` | 448 |
| `process_view.c` | 528 |
| `process_tree.c` | 197 |
| `process_selection.c` | 197 |
| `process_export.c` | 118 |
| `process_hint.c` | 74 |
| `process_scroll.c` | 48 |
| `process_actions.c` | 226 |
| `process_menu.c` | 526 |
| `process_menu_ops.c` | 204 |
| `history_view.c` | 318 |
| `history_data.c` | 490 |
| `history_export.c` | 163 |
| `history_provider.c` | 504 |
| `history_parse.c` | 261 |
| `gui_chart.c` | 601 |
| `gui_chart_draw.c` | 413 |
| `gui_chart_interact.c` | 295 |
| `gui_chart_view.c` | 152 |
| `gui_chart_config.c` | 52 |
| `collector_gpu.c` | 527 |
| `collector_gpu_probe.c` | 235 |
| `collector_gpu_nvidia.c` | 289 |
| `collector_io.c` | 265 |
| `collector_io_meta.c` | 554 |
| `fmon_ctl.c` | 464 |
| `fmon_ctl_service.c` | 118 |
| `fmon_ctl_proc.c` | 158 |
| `chart_geom.c` | 366 |
| `chart_geom_polyline.c` | 207 |
| `chart_geom_cairo.c` | 125 |
| `history_store.c` | 430 |
| `history_sidecars.c` | 242 |
| `combo_chart_draw.c` | 411 |
| `combo_chart_hover.c` | 220 |
| `collector_net.c` | 373 |
| `collector_net_detail.c` | 272 |
| `gui_page_gpu.c` | 335 |
| `gui_page_gpu_detail.c` | 265 |
| `storage_view.c` | 400 |
| `storage_columns.c` | 270 |
| `disk_history.c` | 312 |
| `disk_history_store.c` | 348 |
| `disk_history_draw.c` | 296 |
| `gui_page_prune.c` | 291 |

依赖方向保持单向：

```
gui_window.c -> gui_window_state.c
gui_window_events.c -> gui_window_state.c
log_view.c -> log_filter.c / log_sources.c
gui_pages_layout.c / gui_refresh.c -> gui_page_prune.c
```

## 3. 仍需继续拆分的文件（按优先级）

| 文件 | 行数 | 建议拆分 |
|---|---|---|
| `views/service_detail.c` | 586 | 详情/状态/日志可继续分层 |
| `views/network_view.c` | 563 | 模型刷新/地址解析可继续拆为 `network_model.c` |
| `views/process_view.c` | 528 | 刷新调度可继续拆为 `process_refresh.c` |

这些文件目前功能完整，但超过 1000 行，应作为下一轮结构整理对象；
不要在一次提交中同时移动数据结构和 UI 回调，按“纯函数 → 数据层 → UI 层”
逐步拆分并保持每步可编译。

## 4. C11 / FreeBSD 检查项

| 检查 | 结果 |
|---|---|
| `-std=c11` | 通过 |
| `-Wall -Wextra -Wpedantic` | 0 warning |
| `-Wshadow -Wstrict-prototypes -Wmissing-prototypes` | 0 warning |
| `-Wvla` | 无 VLA |
| 危险字符串函数 | 无 `strcpy/sprintf/gets` |
| 固定宽度整数 | 共享内存 ABI 使用 `uint32_t/uint64_t` |
| 外部命令 | argv + 超时，无 shell 拼接 |
| 线程 | 采集线程只读系统信息，GTK 只在主线程 |
| 共享内存 | seqlock 快照 + ABI/大小校验 |
| 头文件保护 | 全部包含 guard |

## 5. 已记录的架构例外

1. **i18n UI 边界**：为兼容历史未标记中文，GUI 通过 `-include i18n_ui.h`
   在 GTK/Cairo 文本出口统一翻译，并用生成短语表处理拼接文本。
   这是边界层，不是页面逻辑；长期应把源字符串逐步改为 `_()`，
   再删除短语回退。
2. **GTK 退出**：进程在 close-request/SIGTERM 中完成状态落盘后直接
   `exit(0)`，绕过 GTK 4.20 的控件树析构崩溃；这是已知上游问题下的
   隔离边界。
3. **X11 居中**：绝对定位只在 X11 分支编译/执行；Wayland 由合成器决定位置。

## 6. 验证

```
ninja -C build                 0 warning
meson test -C build            20/20（新增 log_filter）
./tests/check_no_bezier.sh .   PASS
git diff --check               PASS
```
