# 模块结构（fmon4）

## 原则

- 一个功能一个 `.c` + 同名 `.h`，模块内状态一律 `static`；
- 模块只通过头文件暴露最小接口，不反向依赖 `main.c` 的私有全局；
- 页面模块的数据来源统一为 `gui_state_snapshot()`（seqlock 一致快照）；
- 依赖方向：`main → 页面模块 → gui_widgets/gui_util/gui_css → gui_state`。

## GUI（src/gui/{app,charts,pages,views,support}）

- `app/`：启动、导航、样式（外壳下一步拆出 gui_window.c）
- `charts/`：单曲线与分带曲线
- `pages/`：CPU/GPU/内存/网络/磁盘页面与指标 updater
- `views/`：进程/服务/存储/网络/日志/告警/限制/历史等列表视图
- `assets/`：内置 SVG 图标（编进 GResource，不依赖桌面图标主题）
- `support/`：控件、工具、主题、告警、控制通道、快照接口

| 模块 | 职责 |
|---|---|
| `main.c`（59 行） | 进程入口：字体/locale 初始化、命令行选项注册 |
| `gui_window.c` | 应用外壳：主窗口、侧边栏、页面栈、刷新调度、对话框接线 |
| `gui_window_state.c` | 窗口大小持久化、最大化处理、X11 居中 |
| `gui_snapshot.c` | 共享内存快照、自动重连、刷新定时器、退出收尾 |
| `gui_runtime.c` | 连接横幅、顶部系统信息、告警指示 |
| `gui_export.c` | CSV 导出 |
| `gui_theme_ui.c` | 主题下拉框、主题切换重绘 |
| `gui_window_events.c` | 关闭/退出信号/快捷键；退出前停定时器、落盘状态并直接结束进程（规避 GTK teardown） |
| `gui_css.c` | 样式表加载器（拼接 4 个片段） |
| `gui_css_main/tables/win11/win11b.c` | 四段样式片段（布局/表格/控件/补充） |
| `gui_widgets.c` | 详情单元格、信息卡、内容签名、网格清空 |
| `gui_pages.c` | `MetricPage`/`ComboPage` 类型 + 指标页 updater + 指标曲线注册表 |
| `gui_page_cpu*`（gui_coreview.c） | CPU 线程方块：布局、绘制、悬停、tooltip |
| `gui_page_gpu.c` | 每张 GPU 独立页面（曲线 + 详情） |
| `gui_page_net.c` | 网络接口详情卡片与刷新 |
| `gui_page_disk.c` | 磁盘设备详情卡片与刷新 |
| `gui_page_mem.c` | 内存条卡片与刷新 |
| `mem_summary.c` | 内存页“已用 / ARC 可回收 / 实际可用”文案（纯函数，有单测） |
| `net_stats.c` | 网络链路统计（getifaddrs/if_data：字节、错误、丢包、载波）+ TCP 重传/监听溢出（net.inet.tcp.stats） |
| `kmem_stats.c` | 内核内存（UMA）汇总：每 5 秒异步跑 `vmstat -z`，归类 ZFS/ZIO/页表；排除 `vm pgcache`（页面缓存不是内核内存） |
| `cpu_stats.c` | CPU 时间分布（kern.cp_times）、每核频率、cs/in 速率与负载（GUI 侧 1 Hz，无 ABI 变更） |
| `gui_page_cpu.c` | CPU 页扩展：时间分布堆叠条、频率、调度压力（并入 CPU 大卡片） |
| `gui_page_l2arc.c` | L2ARC 明细页（设备数/大小/命中/读写/错误/驱逐/重建；未配置时明确提示） |
| `zfs_stats.c` | ZFS ARC 细节（kstat：MFU/MRU、命中分类、回收压力、L2 流量；GUI 侧 1 Hz 采样，无 ZFS 时各页显示“—”） |
| `vm_stats.c` | 内存队列（Active/Inactive/Laundry/Wired/Cache/Free）与 re/sr/po 速率；GUI 直接读 sysctl，给不出数据时整卡隐藏 |
| `gui_nav.c` | 导航交互：分组/子页/启动定位/切换回调 |
| `gui_nav_pages.c` | 页面表与分组表（只读数据 + 查询接口） |
| `gui_export.c` | CSV 导出（异步保存对话框） |
| `gui_page_state.c` | 页面对象唯一持有者（指标页、组合页、曲线、卡片区域） |
| `gui_page_build.c` | 页面骨架构建器（指标页/组合页/滚动包装） |
| `gui_pages_layout.c` | 所有页面创建与 GtkStack 装配 |
| `gui_refresh.c` | 每秒页面刷新与主题重绘（所有 updater 集中于此） |
| `chart_geom.c` | 纯 C 图表几何：量程不裁峰、真实时间轴、NaN 缺口、保峰值抽取、Cairo 路径/极值带 |
| `gui_chart.c` | 单曲线绘图（线性折线、网格、坐标、悬停、峰值） |
| `gui_chart_util.c` | 时间标签、坐标轴文本、速率/数值格式 |
| `combo_chart.c` | 多曲线分带绘图入口（绘制/悬停事件） |
| `combo_chart_geom.c` | 几何布局、每条带量程、数值文本 |
| `combo_chart_draw.c` | 网格/坐标/带框/线性曲线/悬停绘制 |
| `gui_util.c` | 面板绘制、字节/速率格式化 |
| `theme.c` | 明暗主题、调色板、持久化 |
| `gui_state.h` | 界面共享快照访问接口 |
| `gui_ctl.c` | 控制通道（进程/服务操作的 root 通道） |
| `views/process_view.c` | 进程页装配：工具栏、刷新调度、选择保持、CSV 导出 |
| `views/process_ps.c` | ps 输出解析 + `kern.proc.all` 的 WCPU 聚合（有单测） |
| `views/process_model.c` | ProcItem、视图过滤、列排序 |
| `views/process_actions.c` | 信号与 renice、右键菜单、行激活 |
| `views/process_windows.c` | 进程详情窗口、lsof / sockstat 文本窗口 |
| `views/process_list.c` | 进程列表模型栈、ColumnView 列与单元格工厂 |
| `views/service_view.c` | 服务页装配：工具栏、notebook、`service -e` 刷新调度 |
| `views/service_rc.c` | rc.conf / rc.conf.d 解析（纯 GLib，有单测） |
| `views/service_model.c` | rc.d 脚本扫描、第三方服务判定 |
| `views/service_actions.c` | 服务启停 / 开机自启操作、服务详情窗口 |
| `views/service_list.c` | 服务行构建、列表重建、搜索过滤 |
| `views/` 其余（storage/network/log/alert/rctl/history） | 各功能视图 |
| `assets/scalable/actions/fmon-*-symbolic.svg` | 导航与横幅图标（20 个，symbolic 着色，跟随明暗主题） |
| `alerts.c` / `about.c` / `font_setup.c` / `cpuset_dialog.c` | 告警、关于、字体、CPU 亲和性对话框 |

## daemon（src/daemon/{core,collectors}）

- `core/`：进程入口、共享内存、控制通道、历史落盘、采集器入口声明
- `collectors/`：8 个独立采集器

| 模块 | 职责 |
|---|---|
| `fmon_daemon.c` | 启动、共享内存、线程编排、发布/退出 |
| `collectors.h` | 各采集线程入口声明 |
| `collector_cpu.c` | CPU/每核/温度/内存/负载/电池 |
| `collector_io.c` | devstat 磁盘 I/O + 磁盘元数据 |
| `disk_smart.c` | 磁盘温度 provider 链：NVMe ioctl / CAM ATAIO / SCSI LOG SENSE，失败回退 base/可选工具 |
| `disk_meta.c` | 磁盘元数据内核 provider：DIOCG* / NVMe controller / CAM ATA/SCSI；失败回退 geom/diskinfo |
| `collector_net.c` | 接口统计/地址/硬件信息 |
| `collector_gpu.c` | AMD(amdgpu ioctl) / NVIDIA(nvidia-smi) 探测与采集 |
| `collector_zfs.c` | zpool/ARC/命中率/带宽/Prune |
| `collector_meminfo.c` | dmidecode 内存条信息 |
| `collector_storage.c` | 挂载点与 Swap |
| `collector_proc.c` | 进程快照（kvm） |
| `fmon_ctl.c` | 控制 socket（GUI 特权操作） |
| `history_store.c` | 历史曲线落盘 |
| `fmon_sysctl.h` | sysctl 读取辅助（inline） |

## common（src/common）

| 模块 | 职责 |
|---|---|
| `fmon_shm.c/h` | 共享内存布局、seqlock、ABI 校验 |
| `fmon_child.c/h` | 子进程执行与超时保护 |
| `fmon_smart.c/h` | NVMe SMART 文本解析（daemon 采集、GUI 共用） |
