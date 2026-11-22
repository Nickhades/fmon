# fmon 折线图设计说明

> 目标：在 FreeBSD 原生 GTK4/Cairo 技术栈内，画出“专业监控折线”，
> 同时保证曲线上的每个像素都能追溯到真实采样值。

## 1. 为什么不是“平滑曲线”

监控数据是离散采样。渲染层做样条插值、EMA、突变过滤或百分位截断，
都会生成原数据中不存在的中间值，甚至把尖峰削平。对系统监控来说，
这会让用户误判“磁盘没有突发写入”“CPU 没有瞬时打满”。

因此本项目所有时间序列统一采用**纯正直角阶跃线**：

- 一次采样到下一次采样之间，水平保持；下一采样到达时垂直跳变；
- 不使用直线插值、不使用 Bézier、不使用样条；
- 没有采样（NaN）的地方断开，绝不补 0；
- 不在绘图阶段做任何滤波；
- 数据线不做圆角；
- UI 面板采用直角、1px 边框、无阴影；
- 图表使用纯色线条，不使用渐变/面积填充；
- 采集侧也不对温度做 EMA/滑动平均。传感器读数就是监控对象。

## 2. 坐标轴：固定量程不能变成裁剪线

`CurveData.max_val` / `min_val` 是“期望范围”，不是硬裁剪边界。

- 0–100% 的指标默认固定 0–100，曲线位置可以直接横向比较；
- 如果真实值超出期望范围（例如温度 >100°C），坐标轴会自动扩展；
- 不再使用 `v > max ? max : v` 这类静默裁剪；
- 轴刻度用 1 / 2 / 2.5 / 5 / 10 × 10^n 的“好看”档位，
  小数位由刻度间隔决定（0.25 显示两位，2.5 显示一位）。

### 可选自动量程

百分比图表默认 0–100；如果用户需要看低负载时的细节，可在右上角
“…”菜单打开 **百分比图表自动量程**。打开后：

- 只有 `unit == "%"` 的图表忽略固定 0–100 基准，按窗口数据缩放；
- 轴标签仍显示真实数值，并在图内标出“自动量程”；
- 设置持久化在 `$XDG_CONFIG_HOME/fmon/chart_auto_range`；
- 默认关闭，避免把 0.2% 的抖动画成满屏。

实现：`src/gui/charts/chart_geom.c` 的 `fmon_chart_scale()`，
设置界面在 `src/gui/app/gui_window.c`。

## 2.1 实时图时间窗口

主界面右上角提供 `1 分钟 / 5 分钟 / 15 分钟 / 20 分钟` 选择器：

- 只裁剪**显示范围**，不改共享内存中的原始采样；
- Y 轴只按窗口内有限采样重新计算，窗口外的尖峰不会把当前视图压扁；
- 统计图例也只统计窗口内数据；
- 20 分钟是 SHM 环形缓冲上限，不在 SHM 里伪造更长历史。

1 小时/24 小时视图使用“历史”页：它读取 daemon 写入的
`history.tsv`，提供 5 分钟、15 分钟、1 小时、24 小时，
24 小时视图保留桶内 min/max 包络。

### 交互

- **滚轮**：在 1m / 5m / 15m / 20m 四档间缩放，顶部选择器同步；
- **左键框选**：拖出蓝色选区，松开后把选区时间范围放大。选区过窄（<5px）时忽略；
- **右键拖动**：时间轴平移。向右拖看更早，向左拖靠近最新；
- **双击**：回到最新位置和 20 分钟窗口；
- 框选/滚轮得到的非预设窗口在选择器中显示为“自定义”；
- 平移/框选时 X 轴末端显示相对当前时刻的偏移（例如 `-27s`），不再把历史窗口错写成“现在”；
- 交互不会修改任何采样值，只改变可视时间范围。

## 3. 真实时间轴与缺口

共享内存 ABI 21/22 新增逐点时间戳；ABI 23 增加探测有效位/磁盘元数据；ABI 24 扩大网卡型号字段。历史布局保持一致：

```c
uint64_t history_time_us[FMON_HISTORY_POINTS];  /* CLOCK_MONOTONIC */
```

- 发布线程每秒写入一个时间戳，与 `history_*` 数组共用环形索引；
- 绘图按 `(t - t0) / (t1 - t0)` 映射 x，不再假设“每个点严格相隔 1 秒”；
- 相邻采样时间差超过 2.5 个标称间隔时折线断开，daemon 停摆会留下真实空白；
- 时间戳缺失/全为 0 时退回 `interval_us` 等间距，兼容界面内自建的派生曲线；
- 历史页从 TSV 读取每行时间戳，桶时间取桶内最后一个采样。

实现：`fmon_chart_time_range()`、`fmon_chart_retime()`、
`fmon_chart_polyline_at()`。

> ABI 24：升级后必须同时重启 `fmon-daemon` 与 `fmon-gui`，旧 GUI 会被拒绝。

## 4. 缺口：NaN 是“没有采样”，不是 0

共享内存历史数组约定：

| 值 | 含义 | 绘图行为 |
|---|---|---|
| 有限数值 | 有效采样 | 正常连线 |
| `NaN` | 传感器不存在 / 读取失败 / 该指标未配置 | 断开折线，CSV 留空 |

守护进程只在有真实数据时写数值；例如：

- 无电池：`battery_history = NaN`；
- 无 L2ARC：`zfs_l2_size_history = NaN`；
- 无 GPU 负载传感器：`gpu_history = NaN`；
- 无 CPU 温度传感器：`temp_history = NaN`。

这样 “0%” 和 “没有传感器” 不会在曲线上变成同一个东西。

## 5. 像素少于采样点：保峰值抽取

5 分钟 1Hz 是 300 点；20 分钟是 1200 点。窗口宽度可能只有 700–1000 px。

当点数多于像素列时，不能简单隔点抽样（会漏掉尖峰）。实现按像素列分桶：

1. 每个桶内找第一个、最后一个、最小、最大四个**真实**采样索引；
2. 去重后按时间顺序输出；
3. 连续有效段之间插入 `gap`，缺口不连线。

有 min/max 包络时禁用像素抽取，保证平均线与包络使用同一组采样点。

实现：`fmon_chart_polyline()` / `fmon_chart_polyline_at()`。

## 6. 24 小时历史：平均线 + 极值带

`history.tsv` 每 10 秒一行。历史页按时间桶聚合：

- `history` = 桶内平均值（主折线）；
- `history_min` / `history_max` = 桶内原始采样的最小/最大值（半透明带 + 上下边界）；
- 摘要里的“原始峰值”来自全部原始点，不受桶平均影响。

这样 24h 视图既能看出趋势，也不会把瞬时尖峰藏掉。1 小时且点数不足 1200 时
桶宽为 1，极值带退化为一条线，这是正常行为。

实现：`src/gui/views/history_view.c`、`fmon_chart_cairo_band()`。

历史页支持：

- 24 小时视图优先读取 daemon 生成的 `history_1m.tsv`（1 分钟 avg/min/max 金字塔），覆盖不足时自动退回原始 10 秒文件；
- 滚轮在 5 分钟 / 15 分钟 / 1 小时 / 24 小时之间切换；
- 左键框选局部时间窗口，双击图表恢复所选范围；
- “导出 CSV”导出当前范围（含框选范围）的平均值与真实 min/max，并附带绝对时间和相对偏移；
- CSV 使用 `,` 分隔，数值固定小数点，不受 locale 影响。

## 6.1 磁盘 SMART 温度：内核优先，工具回退

`/dev/nvme0` 在 FreeBSD 上是 `crw------- root:wheel`，普通用户 GUI 直接打开
只会得到 `Permission denied`。因此磁盘温度必须由 root daemon 采集。
provider 顺序固定为：**内核 → FreeBSD base 工具 → 可选工具**，
设备名来自 `kern.disks`，不写死 `ndaN → nvmeN`。

| 顺序 | 接口 | 实现 |
|---|---|---|
| 1 | NVMe | `/dev/ndaN` → `NVME_GET_NSID` → `/dev/nvmeX` + `NVME_PASSTHROUGH_CMD` GET_LOG_PAGE 0x02 |
| 1 | ATA/SATA | libcam `CAM ATAIO` + SMART READ DATA（0xB0/0xD0/0xC24F） |
| 1 | SCSI/SAS | libcam `CAM SCSI LOG SENSE` page 0x0D |
| 2 | NVMe 回退 | base `nvmecontrol logpage -p 2`，失败才用 |
| 3 | 可选回退 | `smartctl -A`（smartmontools 安装后才用），覆盖部分 SATA/USB/RAID |
| — | 全失败 | `smart_valid=0`，显示 “—”，绝不伪造 0°C |

- daemon 每 30 秒探测一次并缓存，结果写入
  `FmonDiskInfo.temp_c / spare_pct / wear_pct / smart_valid / smart_source`；
- GUI 只读快照：温度曲线、磁盘明细表“温度”列、存储页 SMART 列均来自 daemon；
- GUI 不直接访问 `/dev/nvme0`，普通用户不会再有权限错误；
- 解析器是纯 C 的 `src/common/fmon_smart.c`，首选读内核二进制页；
  只有工具回退才解析文本；
- `install.sh` 检测 `dmidecode`、`smartctl`、`libdrm`：系统已有则跳过，缺失则自动安装（可用 `--skip-deps` 跳过）；
- 没有标准温度接口的 RAID/虚拟盘显示 “—”，不伪造。

## 6.2 主界面 1 小时 / 24 小时长历史

顶部时间窗口新增 `1 小时` / `24 小时` / `7 天` / `30 天`：

- 数据来自 GUI 侧 `history_provider`，按范围读取 1m/5m/1h 层
- 不把 24 小时数据塞进共享内存
- 支持 CPU、内存、Swap、网络收发、磁盘读写、CPU 温度
- 支持 ZFS 池使用率/ARC 命中率/L2ARC/ZFS IO
- 支持 GPU 利用率/温度/显存（按 slot；1m sidecar）
- 磁盘温度、ZFS/GPU sidecar 缺失时明确不可用
- 1h/24h 使用 1 分钟层；7d 使用 5 分钟层；30d 使用 1 小时层
- CPU/内存/温度保留各层桶内 min/max 包络
- 网络/磁盘使用 1 分钟平均值
- ZFS 容量构成、GPU 详情等非历史项保持实时；界面显示长历史范围提示
- 长历史每 60 秒重新加载一次，不伪装成 1Hz 实时数据

daemon 同时生成：

```text
history_1m.tsv  1 分钟层
history_5m.tsv  5 分钟层
history_1h.tsv  1 小时层
```

1 分钟层字段：

```text
epoch cpu_avg cpu_min cpu_max mem_avg mem_min mem_max swap_avg
net_rx_avg net_tx_avg disk_rd_avg disk_wr_avg
temp_avg temp_min temp_max disk_temp_avg samples
```

5 分钟/1 小时层用于 7 天/30 天窗口。

ZFS 与 GPU 使用独立 1 分钟 sidecar，避免主表为可变 GPU 数量膨胀：

```text
history_zfs_1m.tsv  epoch zpool_util arc_pct l2_size_gb l2_hit_pct rd_kbps wr_kbps
history_gpu_1m.tsv  epoch slot usage_pct temp_c mem_used_mb
```

主界面已绑定：

- ZFS 池使用率
- ZFS 命中率子页（ARC 命中率）
- zpool 读/写带宽子页
- L2ARC 大小/命中率
- GPU 利用率、温度、显存使用

ZFS 容量构成等仍为实时；长历史提示会明确标注
“ZFS/GPU 部分聚合”。

## 7. 悬停读数：吸附真实采样点

- 十字线吸附到最近的采样点，而不是停在鼠标的任意像素；
- 读数就是该采样点的值，不做线性插值“估算”；
- 有时间戳时按真实时间找最近点，没有时按等间距索引；
- 历史页悬停显示“平均 X [min – max]”；
- 峰值参考线取窗口内真实最大值（含包络）。

## 7.1 告警阈值线与统计图例

图表会读取 `~/.config/fmon/alerts.ini` 的当前阈值，在曲线上画红色虚线：

- CPU 使用率：`cpu_pct`（在半圆仪表上画红色阈值刻度）
- 物理内存：`mem_pct`
- Swap：`swap_pct`
- ZFS 池：`pool_pct`
- 温度：`temp_c`（CPU 温度、磁盘温度）

阈值只在进入当前坐标范围时绘制，不为了“看起来完整”而裁掉或移动刻度。
告警开关关闭时不画线。线条使用虚线，数据折线本身仍为实线、直角转折。

图表同时显示窗口内真实采样的统计值：

```text
min 0 B/s · avg 1.8 KB/s · max 1.22 MB/s
```

多带组合图把统计值放在每条带的标题下方；单曲线放在绘图区左上角。
统计值只对有限数值取样，NaN 不计入 min/avg/max，也不当成 0。

实现：`gui_chart_curve_threshold()`、`draw_threshold()`、`draw_stats()`、
`draw_band_label()`。

阈值穿越点用红色小三角标记；统计图例显示
`min / avg / max / last`，都只使用当前可视窗口内的有限真实采样。
颜色统一由 `support/palette.c` 提供：CPU 蓝、内存绿、磁盘读橙、
磁盘写紫、网络接收青、网络发送粉、温度红、GPU 黄；深色主题使用提亮色。

## 7.2 像素网格对齐

1px 的网格线、分带分隔线使用 `floor(coord) + 0.5` 对齐像素中心，
避免落在两个像素之间产生 2px 灰边。**数据折线不做像素吸附**，因为那会
改变真实采样点的坐标；只对装饰性线条做对齐。

## 8. 单位与格式

- `KB/s`：按 1024 进位显示 B/s、KB/s、MB/s、GB/s；
- `%`、`°C`：固定小数位；
- `次`：整数；
- 所有轴标签的小数位由刻度间隔决定，避免 41.7/41.8 显示成同一个数；
- `NaN` 统一显示 “—”。

实现：`src/gui/charts/gui_chart_util.c`。

## 8.1 降采样缓存

单曲线、组合图和 CPU 自定义平均曲线都使用按绘图区持有的降采样缓存。

缓存 key 必须包含 **`time_us` 指针本身**，不能只比较时间范围或 last timestamp：

- `history` 指针；
- `time_us` 指针；
- `history_min` / `history_max` 指针（如有包络）；
- `count` / `index` / `end_time_us`；
- 视口时间范围、画布尺寸、margin、量程、抽列数、gap 阈值。

`time_us` 指针变化意味着时间轴数组被替换或切换，缓存必须失效；指针相同但内容推进时，由 `count`/`index`/`end_time_us` 触发失效。缓存只保存几何，不缓存 Cairo path/颜色/主题。

实现在 `src/gui/charts/gui_chart.c`、`src/gui/charts/combo_chart.c` 和 `src/gui/pages/gui_page_cpu.c`；对应 match 函数都显式比较 `c->time_us == cd->time_us`。

## 8.2 性能结论

实际 profile：整页刷新约 9~11 ms；1200 点折线/包络不是瓶颈。因此暂不引入 worker thread，避免快照生命周期、宽度变化、请求背压和跨线程发布复杂度。当 24h/7d 长历史点数或 series 数量显著增加时，再引入“主线程请求、worker 纯几何计算、不可变帧发布、主线程 Cairo”的架构。

## 9. 代码结构

| 文件 | 职责 |
|---|---|
| `src/gui/charts/chart_geom.[ch]` | 纯 C 几何：量程、环形缓冲、时间映射、缺口、保峰值抽取、Cairo 路径/极值带 |
| `src/gui/charts/gui_chart.[ch]` | 单曲线卡片（CPU/GPU/ZFS/历史页） |
| `src/gui/charts/combo_chart*.[ch]` | 多带组合图（网络/磁盘/内存；每条带独立量程） |
| `src/gui/charts/gui_chart_util.[ch]` | 轴/时间/数值格式化 |
| `src/gui/pages/gui_page_cpu.c` | CPU 页 1/5/10 分钟平均与温度折线 |
| `src/gui/views/history_view.c` | 历史 TSV 读取、时间桶聚合、min/max 包络 |
| `tests/test_chart.c` | 量程、时间映射、缺口、抽取、极值带、离屏渲染 |

`chart_geom` 只依赖 libm + Cairo，不依赖 GTK，所以可以在无 X11 的环境单测。

## 10. 测试

```sh
meson setup build --buildtype=release
ninja -C build
meson test -C build --print-errorlogs
```

`test_chart` 覆盖：

- 固定 0–100% 仍能扩展轴，不裁 123% 的尖峰；
- 窄幅数据自动量程；
- 真实时间映射与时间缺口；
- NaN 缺口断开；
- 环形缓冲回绕；
- 1000 点抽到 100 列仍保留尖峰；
- 轴标签/速率/时间的格式；
- min/max 包络带与 Cairo image surface 离屏渲染。

## 11. 与 FreeBSD 设计哲学的关系

- 不引入 Electron/Web 图表库/额外守护进程；使用系统已有的 GTK4 + Cairo；
- 纯 C11，遵循 base 编译器与 meson/ninja 构建；
- 子进程统一走 `fmon_child` 的 argv + `posix_spawnp`，不再经过 shell；
- 数据面保持“守护进程写共享内存、GUI 只读快照”的零拷贝模型；
- 安装仍由 `install.sh` + rc.d 管理，不写 systemd/Linux 专用逻辑；
- 绘图代码可单测、可离屏渲染，不依赖桌面会话。

## 12. 已知限制与下一步

1. **时间戳与所有指标共用一套**：当前 `history_time_us` 是每秒一个时间点，
   不是每个指标单独时间戳；这是有意的，因为所有指标由同一发布线程写入。
2. **包络只用于持久化历史页**：实时曲线没有 min/max 辅助数组，因为实时
   数据逐秒采样，任意窗口内的极值就是采样点本身。
3. **超过 1200 点的历史范围**：当前 UI 只提供 1h/24h，24h 已先聚合成
   1200 个桶；若以后支持更长范围，可在聚合层加入 LTTB 或 min/max 双线。
4. **历史文件测试钩子**：`FMON_HISTORY_FILE` 可覆盖 TSV 路径，仅用于
   本地测试/演示，不改变安装行为。
