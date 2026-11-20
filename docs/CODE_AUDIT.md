# C11 代码审核（fmon4）

审核日期：2026-11-11  编译器：clang 19.1.7 / FreeBSD 15.1

## 编译配置

- 语言标准：`c_std=c11`
- 警告：`warning_level=3` 额外开启
  `-Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wpointer-arith -Wwrite-strings -Wvla`
- 加固：`-D_FORTIFY_SOURCE=2 -fstack-protector-strong`，
  链接 `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack`

## 审核结果

| 项目 | 结果 |
|---|---|
| 编译警告 | **0**（GUI + daemon + tests） |
| 单元测试 | 15/15 通过（shm / child / ps / rc.conf / 文案 / 队列 / ZFS / CPU / 网络 / 图表几何与离屏渲染等） |
| 头文件保护 | 全部 `.h` 均有 `#ifndef/#define/#endif` |
| 危险函数 | 无 `strcpy/sprintf/gets/atoi/rand` 调用（仅注释提及） |
| 模块内状态 | 新拆模块统一 `static`，接口经头文件暴露 |
| 数据类型 | 共享内存结构使用固定宽度整数（`uint32_t/uint64_t`），`double` 用于比例/速率 |
| 错误处理 | 子进程走 `fmon_child_run()` 带超时；sysctl/shm 打开失败均返回错误串，不静默 |
| 并发 | 共享内存写者持有写锁；读侧使用 seqlock 快照，避免撕裂 |
| 命名 | 统一 `gui_*` / `fmon_*` 前缀；视图构造函数已改为 `gui_*_view_new`（第 6 轮完成） |
| 魔数 | 时间常量集中在 `fmon_shm.h`（`FMON_SAMPLE_INTERVAL_MS` 等）；绘图内少量像素常量属局部实现细节 |

## 已知待办（不阻塞）

1. `main.c` 作为应用外壳保留：命令行、窗口组装、1 秒刷新调度与退出处理
   （1597 行，占 GUI 代码约 13%）；各页面/功能已全部独立成模块；
2. ~~旧概览卡片页（`sys/hw/zfs`）不可达代码~~：已删除，指标曲线改由
   `gui_pages.c` 的注册表统一提供（第 7 轮完成）；
3. 旧视图模块函数名（`create_process_view` 等）与 `gui_*` 前缀不一致，
   可在不改行为的前提下批量重命名；
4. ~~`gui_upd_gpu` 与旧 GPU 详情路径~~：已删除（第 5 轮）。

## 重构后规模

| 项目 | 重构前 | 重构后 |
|---|---|---|
| `main.c` | 4237 行 | 1597 行 |
| GUI 源文件数 | 12 | 27 |
| daemon 采集器 | 8 | 8（+collectors.h 统一入口）|
| 编译警告 | 0 | 0 |
| 测试 | 2/2 | 15/15（新增图表几何/缺口/抽取/离屏渲染、ps、rc.conf、ZFS、CPU、网络等单测） |

## 2026-11-20 折线图专项复审

### 修复

| 问题 | 位置 | 处理 |
|---|---|---|
| 曲线实际画的是阶梯（step），与“线性折线”目标不符 | `gui_chart.c`、`combo_chart_draw.c`、`gui_page_cpu.c` | 全部改为真实采样点直线连接 |
| 柱形量程用 99.5% 分位，主动裁掉最高 0.5% 尖峰 | `combo_chart_geom.c` | 删除分位裁剪；轴覆盖真实最大值 |
| CPU 平均负载轴上限用 99.5% 分位，尖峰被裁 | `gui_page_cpu.c` | 改用真实最大值 |
| 固定 0–100 时把超出数据钳到 100，静默丢峰值 | `gui_chart.c` | 轴自动扩展；固定值只作基准 |
| NaN/Inf 被改成 0，传感器缺失显示成“0%” | `gui_chart.c`、`combo_chart_draw.c` | NaN 表示缺口，断线；CSV 留空 |
| 无温度/电池/GPU/L2ARC 时历史写 0，伪造“0” | `fmon_daemon.c`、`collector_cpu.c` | 改写 NaN；界面显示不可用 |
| CPU 温度在采集侧做 EMA（0.7 旧值 + 0.3 新值），会延迟/削平真实尖峰 | `collector_cpu.c` | 删除 EMA，写入原始传感器值 |
| 点数多于像素时简单绘制，尖峰可能被抗锯齿吃掉 | `chart_geom.c` | 按像素列保留首/末/最小/最大 |
| 悬停十字线跟鼠标，但读数取左侧采样点，位置与数值不一致 | `combo_chart_draw.c` | 十字线吸附真实采样点 |
| 时间轴刻度用 count×interval，采样缺口时偏移 | 各图表 | 统一用环形缓冲有效点数与真实间隔 |
| 历史页先隔点抽样再做桶平均，两轮抽样漏尖峰 | `history_view.c` | 单次桶平均，平均/峰值基于全部原始点 |
| 组合图当前值取 `count-1`，count 超容量时越界/NaN | `combo_chart_draw.c` | 先钳制到容量 |

### 测试

- 新增 `tests/test_chart.c`：量程不裁峰、NaN 缺口、环形回绕、1000→100 保峰值抽取、轴/速率/时间格式、Cairo 离屏渲染。
- 全量 clean build：**0 warning**。
- `meson test`：**15/15 通过**。
- 在 FreeBSD 15.1 + GTK 4.20.4 实机启动 GUI，抓取 CPU/内存/网络/历史页截图确认线性折线与缺口行为。
- `install.sh`：`sh -n`、`--dry-run` 安装/卸载/--build-only/--prefix 通过；用假 sudo 验证非 root 重新执行时参数不再丢失。

### 安装脚本修复

| 问题 | 后果 | 处理 |
|---|---|---|
| 顶层 `while shift` 吃光参数后再 `exec sudo "$SELF" "$@"` | 非 root 执行 `--uninstall` 会变成安装 | 改为 `parse_args()` 函数内解析，保留原始 `$@` |
| `LOCALEDIR` 从未定义 | 有 .mo 时 `set -u` 直接中止 | 定义 `$PREFIX/share/locale` |
| 先删二进制再 `detect_shm_name` | 卸载清不掉共享内存 | 删除前解析并保存名字 |
| 卸载不删 .mo/许可证 | 残留文件 | 增加清理 |
| nvidia-smi 管道里设置 `found_gpu` | 只有 NVIDIA 时误报未检测到显卡 | 先命令替换取输出，再置标志 |
| `--build-only` 非 root 会尝试 pkg install | 与“普通用户可用”矛盾 | 缺依赖时明确提示 root 安装 |
| `--prefix` 未校验 | `/` 或 `..` 可能造成危险删除 | 只接受绝对路径，拒绝 / 与 .. |
| 依赖检查缺 fontconfig/sqlite3 | meson 配置失败 | 补入检查与安装列表 |
| `--launch` 只认 DISPLAY | Wayland 桌面无法启动 | 同时接受 WAYLAND_DISPLAY |

### 其他

- `Makefile` 仍列出旧版 `src/gui/main.c` 等不存在路径；改为 meson/ninja 薄封装。
- `po/POTFILES.in` 全部指向旧路径，`ninja fmon-pot` 失败；改为实际源文件列表。
- `FMON_HISTORY_POINTS` 注释仍写“4Hz = 5 分钟”，实际为 1Hz = 20 分钟；已改正。

## 2026-11-20 专业化四项

| 项目 | 处理 |
|---|---|
| 逐点时间戳 | 共享内存 ABI 20→22，新增 `history_time_us[FMON_HISTORY_POINTS]`；绘图按真实时间映射 x，采样间隔超过 2.5×标称值断线；历史 TSV 用每行时间戳 |
| 24h 峰值包络 | 历史页每个时间桶同时输出 avg/min/max；主折线为平均，半透明带 + 上下边界显示真实极值；平均线/包络共用同一组点，禁用像素抽取避免错位 |
| 去除 popen | `collector_io.c`、`gui_page_batt.c`、`gui_page_disk.c`、`service_actions.c` 全部改为 `fmon_child_run()` + argv，使用绝对基础系统路径，无 shell |
| 可选自动量程 | 百分比图表默认 0–100；右上角菜单显式打开后按窗口缩放，图内标“自动量程”，状态持久化到 `$XDG_CONFIG_HOME/fmon/chart_auto_range` |
| 磁盘温度 | ABI 22 新增 `FmonDiskInfo.temp_c/spare_pct/wear_pct/smart_valid/smart_source`，ABI 23 加 `meta_valid`；root daemon 先走 NVMe ioctl / CAM ATAIO / SCSI LOG SENSE，失败才回退 base nvmecontrol 或可选 smartctl；GUI 不再直接访问 `/dev/nvme0`；磁盘表新增温度列 |
| 测试 | `test_chart` 新增时间映射、时间缺口、min/max 包络渲染；clean build 0 warning，15/15 通过 |

## 结论

当前代码满足 C11 编译与严格警告要求，未发现未定义行为级别的明显缺陷。
折线图已从“阶梯 + 分位裁剪”改为“线性 + 缺口 + 保峰值抽取”；
安装脚本的参数保留、卸载清理与依赖检查也已修正。
剩余工作是结构性清理（窗口拆分、死代码删除、命名统一），不影响功能与稳定性。
