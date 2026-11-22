# 交接说明：fmon4 后续维护、调试与分发（新会话必读）

> 目标：让下一个会话在**不破坏现有功能**的前提下，逐模块调试、修 bug、改功能，
> 同时严格遵循 C11 与 FreeBSD 规范，并保持“可分发到其他 FreeBSD 15.x”的通用性。

---

## 1. 项目速览

| 项 | 值 |
|---|---|
| 路径 | `FreeBSD图形化系统资源管理器/fmon4` |
| 版本 | 1.3.5 |
| 语言/标准 | C11（`c_std=c11`），clang 19 |
| GUI | GTK 4.20（C，无 C++） |
| 共享内存 ABI | **24**，`/fmon_daemon_shm_v24` |
| 采样 | 1 Hz（`FMON_SAMPLE_INTERVAL_MS 1000`），历史 1200 点，逐点 `history_time_us` |
| 数据源 | daemon 采集 → seqlock 共享内存 → GUI 一致快照 |
| 已验证平台 | FreeBSD 15.1-RELEASE-p6 amd64（AMD 核显 + NVIDIA 独显 + ZFS） |

组件：

- `fmon-daemon`（root 服务）：采集器 + 控制 socket + 历史落盘；
- `fmon-gui`（普通用户可运行）：只读快照，按页面展示；
- `common`：共享内存布局 / 子进程执行（两端共用）。

## 2. 构建、测试、运行、安装

```sh
cd FreeBSD图形化系统资源管理器/fmon4

# 构建（当前构建目录已存在）
meson setup --reconfigure build
ninja -C build            # 必须 0 warning
meson test -C build       # 15/15：shm、child、图表几何/时间轴/离屏渲染、ps、rc.conf、内存、ZFS、CPU、网络等

# 本地调试（用构建产物，daemon 以当前用户运行、ABI 24）
./build/src/daemon/fmon-daemon &
./build/src/gui/fmon-gui --page cpu --theme light

# 安装（会同时更新 GUI + daemon + rc.d）
sudo ./install.sh
sudo service fmon_daemon restart
```

**ABI 铁律**：只要改动 `src/common/fmon_shm.h` 中任何结构体/字段/顺序/大小：

1. `FMON_ABI_VERSION` +1；
2. `FMON_SHM_NAME` 同步改（`v23` → `v24`）；
3. 重装后 **GUI 与 daemon 必须同时更新**，否则 GUI 会以“ABI 不匹配”拒绝连接（这是预期保护）。

本地调试时，若系统服务也在运行，两者可能争用同一 shm：
先 `pkill -f build/src/daemon/fmon-daemon` 再启动构建版；安装版服务不受影响。

## 3. 运行时路径与开关

| 用途 | 路径/变量 |
|---|---|
| 共享内存 | `/fmon_daemon_shm_v24` |
| 控制 socket | `/var/run/fmon-ctl.sock`（GUI 也探测 `/tmp/fmon-ctl.sock`） |
| 历史数据（root） | `/var/db/fmon/history.tsv`（普通用户调试：`~/.local/state/fmon/history.tsv`） |
| 告警阈值 | `~/.config/fmon/alerts.ini` |
| 告警历史 | `~/.local/state/fmon/alerts.log` |
| 主题选择 | `$XDG_CONFIG_HOME/fmon/theme`（默认 `~/.config/fmon/theme`） |
| NVIDIA 工具覆盖 | 环境变量 `FMON_NVIDIA_SMI=/path/to/nvidia-smi` |
| 服务脚本 | `rc.d/fmon_daemon.in` → `$PREFIX/etc/rc.d/fmon_daemon` |

命令行：

```text
fmon-gui   --page <id> [--gpu N] [--theme auto|light|dark] [--version]
fmon-daemon -V|--version    -f|--force（强制接管残留 shm）
```

页面 id（`--page`）：`cpu mem netm disk gpu0 gpu1 gpu2 gpu3 batt history
zpool arc zhit zread zwrite zprune temp storage proc svc log conn alerts
rctl iface zsum zbw`。

## 4. 模块地图（详见 `docs/MODULES.md`）

```
main.c(59) 入口
 └ gui_window.c(335) 窗口/侧栏/页面栈/刷新调度
    ├ gui_pages_layout.c(300) 全部页面创建与栈装配
    ├ gui_nav.c(338) + gui_nav_pages.c(118) 两级导航
    ├ gui_runtime.c(117) 横幅/顶部信息/告警指示
    ├ gui_snapshot.c(154) 快照/8 秒自动重连/定时器/退出收尾
    ├ gui_theme_ui.c(78) 主题下拉
    ├ gui_window_events.c(66) 关闭/退出信号/快捷键
    ├ gui_export.c(151) CSV 导出
    └ pages/
       ├ gui_page_state.c(32) 页面对象唯一持有者
       ├ gui_page_build.c(252) 指标页/组合页构建器
       ├ gui_refresh.c(321) 每秒刷新（所有 updater）
       ├ gui_pages.c(245) 指标曲线注册表
       ├ gui_page_gpu.c / net / disk / mem / coreview
       └ charts/  gui_chart.c + util、combo_chart.c + geom + draw
views/  process_{ps,view,model,list,actions,windows}.c
        service_{rc,view,model,list,actions}.c
        其它：storage/network/log/alert/rctl/history
support/ widgets、util、theme、alerts、gui_ctl、font_setup、cpuset_dialog
assets/  20 个 fmon-*-symbolic.svg（编进 GResource /org/fmon/icons；
         新增页面时要在 fmon-icons.gresource.xml 加条目并改 gui_nav_pages.c 的 icon）
daemon/ core/{fmon_daemon,fmon_ctl,history_store,collectors.h}
        collectors/{cpu,io,net,gpu,zfs,meminfo,storage,proc}
```

依赖方向：`main → window → 页面/导航/运行时 → widgets/util/state → common`。
禁止反向依赖；页面模块不要 include `gui_window.c` 内部状态。

### 调试入口

| 模块 | 快速验证 |
|---|---|
| daemon 采集 | `--version` 看 ABI；`sudo ./build/src/daemon/fmon-daemon` 看日志；停服务看 GUI 横幅 |
| 共享内存 | `meson test -C build`；改结构体后重建两端 |
| 进程页 | `--page proc`：列表/搜索/列排序/右键菜单/详情/lsof/导出 |
| 服务页 | `--page svc`：运行/停止分组、隐藏系统脚本、详情、启停（root） |
| GPU 页 | `--page gpu0`（AMD）、`--page gpu1`（NVIDIA）；看 GTT/VRAM、PCIe、悬停 |
| 内存页 | `--page mem`：曲线 + 内存条卡片（需 daemon root + dmidecode） |
| 网络/磁盘 | `--page netm/disk`：双曲线、摘要行、底部详情卡 |
| ZFS 页 | `--page zpool/arc/zhit/zread/zwrite/zprune/zsum/zbw` |
| 主题 | `--theme dark`；下拉切换后重启确认持久化 |
| 导出 | 右上“导出数据”：CSV 小数点必须为 `.`（用 `LC_ALL=de_DE.UTF-8` 验证） |

## 5. 当前验证状态

- 构建 **0 warning**，`meson test` **15/15**（含图表时间轴/包络、ps、rc.conf、内存、ZFS、CPU、网络等）；
- 内存条信息、磁盘设备详情（型号/容量/类型/SN）都从底部卡片移到曲线卡片
  标题行右侧（`ComboPage.header_right`），页面不再需要滚到底部查看设备信息；
- ZFS 带宽页的累计/操作数直接画在各自曲线带内（`ComboSeries.extra`，
  透明背景、不影响曲线），底部卡片已移除；
- ZFS 各页的键值对改到标题行右侧（`gui_metric_page_new(..., compact=TRUE)`），
  底部不再单独占一块；CPU 温度、电池等页面保持原布局；
- ZFS 五个页面已补细节（数据来自 `kstat.zfs.misc.arcstats.*`，GUI 侧 1 Hz 采样）：
  ARC 页显示 MFU/MRU/元数据/占上限，命中率页显示数据/元数据/预取命中构成与 L2，
  Prune 页显示每秒回收、跳过、空间不足、内存节流，读写带宽页显示 L2 速率，
  池空间页补“空闲”；新增“存储 → L2ARC”页：两条历史曲线（L2 大小 GB、
  命中率 %，**由 daemon 写入 shm 历史数组，ABI 24**，1 Hz）+ 设备数/缓存/实际分配/头部
  开销/命中/读写速率/写入完成率/错误/驱逐/重建/MFU-MRU-预取构成；
  `zfs_l2_size > 0` 时自动绘曲线（下游配了 L2ARC 的机器直接可用），
  未配置时明确提示而不是显示 0；
- 内存页新增“内核内存（UMA）”行：GUI 侧每 5 秒异步跑 `vmstat -z`
  （GSubprocess，不阻塞界面），按区名归类 ZFS（zfs_/dnode/dmu_/abd_/arc_/dbuf_）、
  ZIO（zio_）、页表（vm_/pv_）；**排除 `vm pgcache`**（页面缓存，不是内核内存，
  否则合计会接近物理内存）；纯函数 `kmem_stats_parse` 有单测；
- CPU 页新增（并入大卡片，无新 ABI）：时间分布堆叠条 + 5 列等宽图例、
  “状态与调度”对齐网格（当前/最高频率、cs/in 速率、1 分钟负载、逻辑处理器、
  插槽、虚拟化）；原先底部“运行”卡片已移除，信息并入该网格；
  每核占用仍由原有方块视图展示；
- 彻底移除密码框：进程结束/renice、服务启停、rctl、cpuset 都不再调用
  `pkexec`（`fmon_gui_pkexec()` 已删除）；服务/rctl/cpuset 只走 daemon
  控制通道，失败直接报错“控制通道不可用”，不再弹认证窗口；
- 普通用户可查看“系统 (root)”视图（只读）：切到该视图用 `ps -ax`；
  选中非本人进程时“结束任务”按钮禁用、菜单操作被拒绝并提示
  “需以 root 运行 fmon-gui（sudo/doas）”；cpuset 同样只允许自己的进程；
- 内存页的“内存/Swap 总容量”从底部卡片移到标题行右侧（DIMM 摘要下方），
  底部不再多占一张卡片；曲线区高度 320→380，宽度占满卡片；
- CPU 线程方块换配色：浅蓝（空闲）→ 浅琥珀 → 橙红（满载），文字改深色；
  旧的“深绿底 + 小白字”在低占用时几乎看不清；
- 进程列表新增“类型”列（桌面/用户/系统），默认排序按类型分组：
  桌面应用 → 其他用户进程 → 系统/桌面组件（kwin/plasma/dbus/portal 等），
  组内仍按 CPU 降序；分类纯函数 `proc_classify_category` 在
  `process_ps.c`，按 argv[0]（去掉 ps 的 "name:" 冒号与路径）匹配白名单；
  含单测（chrome/konsole/syncthing/kwin/daemon: 前缀）；
- 修复进程/服务确认框的所有权错误：`fmon_gui_confirm` 会在回调返回后统一释放
  user_data，但 `proc_actions_send_signal` / `do_renice` / `svc_op_confirmed`
  又在回调里释放同一个指针 → double free；异步 pkexec 路径还会在确认框关闭后
  继续使用已释放的 op（use-after-free）。现在回调只借用，异步路径用
  `signal_op_clone` / `svc_op_clone` 的副本，由异步回调负责释放；
  这是“结束进程后界面卡死/崩溃”的根因（旧二进制留下了 fmon-gui.core）；
- 内存页与 CPU 页的数值列都设为定宽（键 84/96px、值 96/118px），
  刷新时名称与列位置不随数字长短抖动；
- 接口地址优先 IPv4：getifaddrs 对 lo0 先返回 ::1，之前直接取第一条会
  显示成 ::1；现在先找 AF_INET，没有才退回 AF_INET6（lo0 显示 127.0.0.1）；
- 网络接口类型与无线信息：链路表加“类型”列（无线/USB 共享/虚拟/回环/以太网，
  按接口名前缀识别 ue*/urndis*/cdce*/ipheth*/wlan*/tap*/bridge* 等）；
  有 wlan* 时额外显示 SSID/信道/协议（每 10 秒异步跑一次 `ifconfig -a` 解析，
  纯函数 `net_stats_parse_ifconfig` 有单测）。wifibox 是 bhyve 的 virtio tap，
  在宿主机看来是普通“以太网”，SSID 在客户机里，宿主机读不到；
- “网络接口”页并入“网络概览”：链路表扩展为 接口/地址/状态/MTU/错误/丢包；
  网络组只剩 网络概览 / 活动连接（iface 页仍保留在 g_pages 中但不再挂导航，
  避免改动下标）；
- ZFS ARC 明细改为三列（大小/上限/占比；目标值/数据/元数据；dnode/dbuf/\n  系统可用/节流），放在曲线下方同一张卡里；\n- ZFS ARC 页改为非紧凑布局：10 项数值从标题行右侧移到曲线下方，
  键/值左对齐（键列 170px），与其它指标页的 detail 行一致；
- ZFS ARC 页扩充为 10 项：总大小/上限/占上限、目标 c、数据缓冲、元数据、
  dnode、dbuf、系统可用内存、memory_throttle_count；MetricPage.vals 从 6
  扩到 12（builder/set_val 同步），否则第 7 项以后会静默丢弃；
  arc_size 对 arc_c 的“双曲线趋势”尚未做（当前为数值）；
- CPU“状态与调度”和 ZFS“池详情”的键统一改为左对齐（定宽 96px），
  值紧跟其后独立左对齐，不再用右对齐键；
- 池详情两栏并排：左栏实时性能（延迟读/写、队列、错误、慢 I/O），
  右栏维护（TXG、ZIL/SLOG、vdev、scrub）；
  TXG 增加最近 16 次已完成 TXG 的 stime 均值/最大值，最近一次 > 2×均值
  时标注“疑似脏数据节流（同步尖峰）”；
- 池详情拆成独立行：延迟（读）/延迟（写）各一行（total 与 disk 分开），
  队列、错误、慢 I/O、TXG、ZIL/SLOG、vdev、scrub 各占一行；
  碎片率与 ARC 命中率移到标题行右上（hw-model 字号）；
- 池详情改为 CPU 页“状态与调度”同款网格：键右对齐定宽 96，短项两对一行
  （队列|碎片率、TXG|ARC 命中率），长项独占一行（延迟、错误、vdev、scrub），新增
  ARC 命中率（zfs_stats）与 ZIL/SLOG 行（zpool status config 里的 logs 段
  自动检测，未配置时写“ZIL 在池内”）；vdev 行来自 iostat -v，��盘池时
  直接列出 mirror-0/radaiz1-0 及其子设备（-H 不保留缩进，暂不画树）；
- ZFS 池页的“池详情”也并入曲线所在大卡片（append 到 area 的父 box），
  整页只剩一张卡，和 CPU/GPU/内存页一致；
- GPU 独立页的“显卡详情”并入同一张大卡片（曲线下方，detail-title +
  两列网格），不再单独占一张卡，与 CPU 页结构一致；
- ZFS 池页改为 ComboPage 大卡片：使用率（快照历史）与“新增错误/秒”
  （界面环形缓冲）两条等宽曲线 + 每带 Y 刻度；池名/状态、已用/总量、
  使用率/空闲移到标题行右上角（hw-model 字号）；下方池详情保留
  延迟/队列/错误/碎片/TXG/vdev/scrub，键宽 92、左对齐，与内存页统一；
- TXG 实际有 kstat 节点：`kstat.zfs.<pool>.txgs`（不是 kstat.zfs.misc！），
  每 15 秒读 `sysctl -n kstat.zfs.<pool>.txgs`，取最后一个 stime>0 的已完成
  TXG，stime 单位 µs→s；页面显示 “txg N · stime X s（最近一次同步）”；
- 池详情下新增“错误趋势（新增错误/秒）”曲线：界面内环形缓冲，每秒记录
  错误计数（读+写+CKSUM+慢 I/O）的增量，参照磁盘温度曲线；
  TXG：本内核没有 txg_stats kstat，页面显示 vfs.zfs.txg.timeout 并注明
  “stime 无 kstat 节点”，不伪造；
- “ZFS 池空间”页改为“池详情”卡片（原右上角容量数据全部下移，字体/键宽
  与内存页统一）：池名/状态、已用/总量、使用率/空闲、延迟、队列、
  错误/慢 I/O、碎片率、TXG、vdev、最近 scrub；
  vdev 级数据来自 `zpool iostat -Hpv -p 1 1`（第一行池、其余 vdev），
  慢 I/O 来自 `zpool status -sv` 的 SLOW 列；
  TXG stime 在本内核没有 kstat 节点，页面明确写“未暴露”，不伪造数值；
  错误趋势曲线尚未做（可参照温度曲线加 GUI 环形缓冲）；
- “ZFS 池空间”页新增“池健康”小节：每 15 秒异步跑
  `zpool status -v`（错误计数/scrub 结果）、`zpool list -Hp -o name,frag`（碎片率）、
  `zpool iostat -H -l -q 1 1`（total/disk 延迟、syncq/asyncq pend/activ 合计）；
  解析函数纯函数化并有单测。尚未做：vdev 级细分、TXG 状态、`zpool status -s` 慢 I/O；
- 内存页说明行（已用/其中 ARC 可回收/实际可用）从页面最底部移到标题下方，
  与副标题同一条左侧对齐；
- 磁盘温度只保留在“温度”曲线带显示：header 的 SMART 行去掉温度，
  避免同一数值出现两次、以及 %.1f / %.2f 两种格式看起来"不一致"；
  format_value_text 对 °C 统一用一位小数；
- 组合图（combo_chart）每个条带左侧新增 0/中/最大 三个刻度，左留白
  48→74px 避免长刻度被裁；内存/网络/磁盘/ZFS 组合页统一生效；
- 磁盘页新增第三条带“温度”：界面内 1200 点环形缓冲、每秒采样 SMART 温度，
  固定 0–100 °C 量程（避免窄范围锯齿）；无 SMART 时不画；
- 磁盘 SMART 与累计量：每 30 秒由 daemon 直接通过 NVMe ioctl / CAM ATAIO / SCSI LOG SENSE 读取
  （普通用户可读，/dev/nvme0 属于 operator 组），显示温度、寿命已用 %、
  可用备用空间 %；累计读写字节直接取 `devstat.bytes[DEVSTAT_READ/WRITE]`
  （compute_statistics 传 NULL 取不到累计值）；解析函数有单测；
- 磁盘页去重：副标题不再显示“繁忙 %”（与右上 devstat 实时行重复），
  只保留累计读次/写次；繁忙度以 devstat 实时值为准；
- 磁盘页新增实时性能行（devstat，标题行右上）：读/写 IOPS、平均延迟 ms、
  繁忙 %、队列长度；注意 devstat_getdevs 需要调用方先 `calloc` 一个
  struct devinfo 并赋给 sti.dinfo，否则返回 -1；
  （SMART 温度/寿命：由 root daemon 直接读内核页，不依赖 smartctl/nvmecontrol；
  IOPS/延迟曲线暂未做，先给数值）
- 导航重排：磁盘移到“存储”组第一位；电池移到“系统”组末尾
  （系统组：进程管理/服务管理/系统日志/电池）；概览组只剩
  CPU/内存/GPU/历史记录；
- 统一硬件型号字体：新增 CSS 类 `.hw-model`（12px, opacity 0.66），
  CPU/内存/磁盘/网络页的型号都放标题行右上角；CPU 型号从副标题移到
  header_right，副标题恢复固定说明；GPU 页型号按用户要求保持原样；
- 网络概览标题行右侧显示网卡硬件型号：用 `small-text`（12px）+ 固定宽度 640
  换行，长型号完整显示且不抢标题；
- 网络概览标题行右侧显示网卡硬件型号（快照 nic_models，右对齐）；
  注意不能用 wrap=TRUE 的 label 放 header_right——wrap 标签在横向盒子里
  自然宽度为 0，会整块不可见，改用 ellipsize；
- 导航：概览里的“网络”移到“网络”组，改名“网络概览”（页面 id 仍是 netm），
  网络组顺序：网络概览 / 活动连接 / 网络接口；
- 去重：概览页底部的“网络接口详情”卡片删除（网络接口页已有完整视图），
  链路表不再重复“收/发”字节列（接口页/副标题已展示），只保留错误、丢包、状态；
- 网络页新增“链路与错误 / TCP 健康度”（并入大卡片）：每接口收/发字节、
  收/发错误、丢包，未连接接口红色标注（用 ifi_link_state 判断载波，
  不是 IFF_RUNNING）；TCP 显示重传率、监听队列溢出、连接尝试/关闭；
  数据来自 getifaddrs + net.inet.tcp.stats，GUI 侧 1 Hz；
- 内存页“内存队列”详情改为三栏并排（回收与压力 / ZFS 缓存 / 内核内存），
  每栏内键左对齐、定宽 82px，数值紧随，列内竖线整齐；
- 内存页“内存队列”卡片改为两栏对齐小节：回收与压力（re/sr/po/pi/flt/需读盘）、
  磁盘 I/O（读/写/IOPS/繁忙）、ZFS 缓存（命中率/MRU/MFU/节流/压缩比）、
  内核内存（UMA 合计/ZFS/ZIO/页表/缓冲）；键右对齐、值左对齐，不再是一行行
  长短不一的说明文字；
- 内存页新增：pi（换入）/ flt（缺页，含“需读盘”硬缺页）速率、
  ZFS 缓存质量（命中率 / MRU-MFU 占比 / 内存节流 / ARC 压缩比）、
  内核内存（UMA）——都在“内存队列”卡片内，无新卡片；
  （磁盘读/写速率、IOPS、繁忙度只在“磁盘”页展示，内存页不再重复）
- 内存告警改为 FreeBSD 口径：可用（free+inactive+cache）+ ZFS ARC 视为可回收，
  有效使用率 = (total - avail - arc) / total（`gui_mem_effective_used_pct`，
  纯函数 + 单测），避免 ARC 把 90% 阈值顶爆；
- 内存页的“内存队列”并入“内存”大卡片（曲线下方，不单独成卡）：
  堆叠条 + 活跃/非活跃/待回收/常驻/ZFS 缓存/空闲 数值（FreeBSD 的
  `v_cache_count` 基本恒为 0，真正的缓存是 ARC；ARC 计在 Wired 里，
  显示时从常驻中扣除、单独成段）；另有一行 `缓冲（Buf）`（vfs.bufspace，
  top 里的 Buf）——这是 FreeBSD 唯一显式计数的“缓存”，文件页缓存没有
  独立计数、分布在活跃/非活跃/空闲里。以及每秒
  re（重新激活）/ sr（守护进程扫描）/ po（换出）；原说明文字已去掉。
  数据由 GUI 直接读 `vm.stats.vm.*`（全局只读、无需 root），**未动 ABI**；
  `v_pdpages`/`v_reactivated` 是 64 位 sysctl，必须用 8 字节缓冲读。
- 内存页在“已用 / 可用”下方增加一行说明：
  `已用 X（其中 ZFS ARC Y 可回收）｜ 实际可用约 Z`，
  数据取快照里已有的 `zfs_arc_size`，未改 ABI；文案生成抽成纯函数
  `pages/mem_summary.c`，由 `fmon_mem_summary` 单测覆盖（无 ZFS/阈值/钳制）；
- 进程页 CPU 列：用 `kern.proc.all` 的 `ki_pctcpu`（与 top 的 WCPU 同源，
  两位小数，多线程按 pid 求和；`ps` 的 `pcpu` 只有一位小数，仅作回退）；
- 进程页：列表填充、选中行在自动刷新后保持（刷新继续、选中跟随 pid）、
  CPU 列排序、右键菜单、
  “进程详细信息”和“打开的文件 (lsof)”窗口均实测通过；
- 服务页：运行/停止分区（46/20，共 156）、详情窗口实测通过；
- 之前各页面（CPU/GPU/内存/网络/磁盘/ZFS/主题）均已实测；
- 破坏性操作（同一用户、无 root）：用 `sleep 300` 经 GUI 实测通过
  SIGSTOP/SIGCONT/SIGTERM 与 renice（nice=10）；SIGKILL、cpuset、服务启停/自启
  仍待 root 下验证；
- 退出：窗口关闭 / SIGTERM / Ctrl+Q 均干净退出（确认框打开时同样 exit 0、无 core）；
- 未覆盖：root 系统视图、进程 CSV 导出。

## 6. 已知限制 / 技术债（按优先级）

1. **破坏性操作部分回归**：同用户的 `SIGSTOP/SIGCONT/SIGTERM/renice` 已用
   `sleep 300` 通过 GUI 实测；`SIGKILL`、`cpuset`、服务启停/自启仍需 root 权限，
   建议后续在 root 会话中逐项补测（不要在生产服务上试）。
2. **root 系统视图**：`--page proc` 的“系统 (root)”标签页需要 root 运行 GUI 才可用。
3. **进程页 CSV 导出**未实际落盘验证。
4. **`on_ps_done` 是重建版**（历史事故）：数据变化时整体重建 GListStore，
   已验证选中保持；如未来改动刷新逻辑，务必回归“选中不被重置”。
   2026-11-11 起：重建改为 `g_list_store_splice()` 一次性替换（少发 N 次模型信号）；
   选中行时**继续**自动刷新，但视口固定在用户当前滚动位置：重建后按 pid 恢复选中，
   并在其后约 120 ms 内反复把垂直 adjustment 钉回刷新前的值（GTK 的
   “滚动到选中行”是延迟到布局阶段生效的，单次 set_value 会被覆盖）；
   提示栏显示“已选中 PID xxx，自动刷新时视口保持不动”；切回页面时立即刷新一次。
   注意：选中项若滚出视口，仍保持选中（底部按钮指向它），只是看不到高亮。
   刷新间隔保持 **3 秒**（刻意选择：65 进程约 10–15 ms/轮，1 秒间隔会多 1–4% 单核占用）。
5. **解析器单测已补齐一部分**：新增 `fmon_process_ps`、`fmon_service_rc`
   两个纯 GLib 单测（共 4/4）；dmidecode、zpool/ARC、CSV 写出仍建议补纯函数单测，
   新解析逻辑一律先抽成不依赖 GTK 的 .c 再测。
6. **GPU 字段依赖驱动**：消费级 NVIDIA 的序列号/风扇/电压可能 `N/A`（界面已隐藏）；
   AMD 的 SM 数量、电压/电流通常不可得；不要把“—”当成 bug。
7. **dmidecode 需要 root**：内存条卡片只有 daemon（root）采集才有数据；
   普通用户运行 GUI 时卡片会提示需要 root。
8. **终端/桌面依赖**：字体渲染强制 CPU（`font_setup.c`），通知依赖 `notify-send`；
   `wifibox` 这类 VM Wi-Fi 接口没有 SSID/信道（属正常）。
9. **退出策略**：窗口关闭直接 `exit(0)`（GTK 4.20 teardown 崩溃规避），
   不要在 close 路径里恢复控件销毁，否则可能重新引入 SIGBUS。
   `SIGTERM/SIGINT/Ctrl+Q` 也走 `quit_now()`（`gui_runtime_shutdown()` +
   `exit(0)`），不再调用 `gtk_window_close()`——曾有模态确认框打开时收到
   SIGTERM、GTK 进入控件树销毁并产生 core 的案例；改后实测干净退出（无 core、
   无 Gtk-CRITICAL）。
10. **详情标签不可选中**：为规避“选中标签被每秒重建 → 复制时 SIGBUS”，
    详情值标签关闭了 selectable；改为池化更新前不要重新打开。

## 7. C11 / FreeBSD 代码规范（强制）

编译已开启：`c_std=c11`、`warning_level=3`、`-Wformat=2 -Wshadow
-Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings
-Wvla`，以及 `_FORTIFY_SOURCE=2`、栈保护、RELRO/NOW/noexecstack。
**任何提交必须 0 warning。**

### 必须

- 模块内符号 `static`；跨文件接口进头文件并加 `#ifndef` 保护；
- 函数声明带完整原型；不使用 K&R 声明、不使用隐式声明；
- 固定宽度整数（`uint32_t/uint64_t`）用于共享内存/二进制数据；`double` 用于比例、速率；
- 字符串用 `strlcpy/strlcat`，禁止 `strcpy/sprintf/gets`；
- 所有系统调用/库调用检查返回值；错误向上传递或写日志，不静默吞掉；
- 解析外部命令输出时：`LC_NUMERIC=C`，用 `g_ascii_strtod` / `sscanf("%lf")` 前确认 locale；
- 共享内存写入必须持写锁；读侧只用 `fmon_shm_snapshot()` 一致快照；
- 子进程一律走 `fmon_child_run()` / `fmon_gui_run_async*()`（带超时、无 shell）；
- GTK 控件只在主线程操作；长命令禁止同步阻塞主循环；
- 缩进 4 空格、行宽 ≤ 80（与现有代码一致），注释用中文/英文均可但要解释“为什么”。

### 禁止

- VLA（`-Wvla`）、GNU 扩展（`typeof`、语句表达式等）；
- 在 GTK 回调里销毁正在被引用的控件；每秒重建网格时不要销毁有选中的控件；
- 未经 ABI 升级修改 `fmon_shm.h` 布局；
- 硬编码 `/usr/local`：使用 `FMON_LOCALBASE`（meson 注入）或 `PATH` 查找；
- 假设 ZFS/NVIDIA/dmidecode/pciconf/battery 一定存在——全部按“可选”处理并降级显示；
- 用正则/脚本一次性大段切割源码（本项目已因此误删过函数）。

### FreeBSD 注意点

- 使用 `sysctlbyname`、`getifaddrs`、`devstat(3)`、`kvm(3)`、`diskinfo`、`pciconf` 等原生接口；
- `kvm`/`dmidecode`/`ifconfig` 的权限差异：daemon 以 root 运行，GUI 普通用户；
- 子进程查找走 `PATH`（`install.sh` 已把 `/usr/local/sbin` 纳入服务环境）；
- 新增依赖要更新 `install.sh` 的 `PKG_DEPS` 与 README 依赖说明；
- 新增可选工具（如 `lsof`/`sockstat`）要有“缺失时”的降级路径；
- 不要在代码中假定 FreeBSD 15.1——使用 `kern.osrelease` 展示即可，接口按 15.x 通用实现。

## 8. 分发通用性检查清单

发布前逐项确认：

- [ ] `./install.sh --prefix /opt/fmon` 可完整安装（含 rc.d 的 `@PREFIX@` 替换）；
- [ ] 目标机无 ZFS：ZFS 相关页显示“无可用池/不可用”，不崩溃；
- [ ] 目标机无 NVIDIA：只出现 AMD/Intel 页；`--gpu 1` 不越界；
- [ ] 目标机无 `dmidecode`：内存条卡片提示“需要 root/未安装”，其余正常；
- [ ] 目标机无 `notify-send`：告警仅界面提示，无崩溃；
- [ ] 首次运行无配置文件：默认主题（跟随系统）、默认告警阈值；
- [ ] `LC_ALL=de_DE.UTF-8` 下 CSV 小数点为 `.`；
- [ ] GUI 与 daemon 版本/ABI 不一致时给出明确提示而不是崩溃/读越界；
- [ ] 服务重启后 GUI 8 秒内自动重连；
- [ ] 关闭窗口、SIGTERM、Ctrl+Q 均干净退出，无 core。

## 9. 改代码的推荐流程

1. 先跑 `ninja -C build && meson test -C build` 记录基线；
2. 小步修改，每次只动一个模块；改完立即重编 + 跑相关页面冒烟；
3. 任何共享内存/协议改动：ABI+1、重装两端、验证旧 GUI 会被拒绝；
4. 更新 `docs/MODULES.md`（模块）；
5. 提交前执行第 8 节清单，并在提交说明中记录验证结果。

## 10. 新会话开场提示词（可直接粘贴）

```
继续维护 FreeBSD监控中心(fmon4)。
项目：/home/zxy/项目/FreeBSD图形化系统资源管理器/fmon4
先读 docs/HANDOFF_NEXT_SESSION.md、docs/MODULES.md、docs/PROJECT_STRUCTURE.md。
已落地（阶段 1）：GUI 常驻采集线程
- src/gui/pages/collector_thread.c：单线程 1 Hz 采集 zfs/cpu/net/disk，
  经 g_idle_add 整帧发布；主循环繁忙时最多挂 2 帧、旧帧丢弃；
- 各模块拆成 collect（工作线程只读）/ publish（主线程发布）：
  zfs_stats、cpu_stats、net_stats、disk_perf；
- FMON_LEGACY_COLLECT=1 回退旧路径；关闭窗口/SIGTERM 时 join 线程；
- 冒烟：zpool/cpu/netm/arc 页数据正常，旧路径回退正常。

原计划（阶段 2/3 未开工）
- 阶段 1：仅 GUI，单常驻采集线程搬 zfs_stats/cpu_stats/net_stats/disk_perf；
  双缓冲整帧发布；环境变量 FMON_LEGACY_COLLECT=1 回退旧路径；
- 验收：1 小时 soak、慢调用看门狗归零、页面数据抽查一致、关闭不卡死；
- 阶段 2：采集线程加 _Thread_local 日志前缀/错误码/耗时（不参与同步）；
- 阶段 3（可选）：daemon 只加 TLS 日志与耗时统计，不动调度/发布。

内存增长排查结论（2026-11）：
- 6h soak：RSS 307→452 MB（约 24 MB/h，线性）；进程存活、线程数稳定；
- procstat -v：映射数/线程数不变，增长集中在 anon:swap 的 resident；
- jemalloc stats.allocated/active/resident 在 10 分钟内完全不变 → 不是 malloc 泄漏；
- A/B：FMON_NO_PAGES=1 时 RSS 完全持平；FMON_NO_REDRAW=1 时约 +1MB/8min；
  FMON_NO_CHART_DRAW=1 与全量相同；FMON_DRAW_EVERY=5 也与全量相同
  → 增长与重绘次数无关，来自每秒刷新触发的 GTK 文本/布局缓存，不是我们的
  cairo 绘制代码，也不在应用堆上；
- 结论：属 GTK 渲染/文本缓存行为（约 15–25 MB/h）。可选缓解：降低标签更新
  频率、定时重启；调试开关（默认关闭）：FMON_REFRESH_STATS、FMON_HEAP_STATS、
  FMON_NO_PAGES、FMON_NO_REDRAW、FMON_NO_CHART_DRAW、FMON_DRAW_EVERY=N。

FreeBSD 共享内存/同步审计（2026-11）：
- 跨进程同步只用 seqlock，不用 PTHREAD_PROCESS_SHARED 互斥锁，
  进程崩溃不会留下“永久锁”；daemon 的 pthread_mutex 仅在进程内、
  保护同一发布线程，不跨进程；
- 创建路径 shm_open(O_CREAT|O_RDWR) → ftruncate(sizeof(FmonShmData))
  → mmap(MAP_SHARED)，读端先校验对象大小再 mmap；
- 读端 mmap 后调用 madvise(MADV_SEQUENTIAL)（整结构顺序拷贝）；
- 不调用 fsync（FreeBSD POSIX shm 不支持，调用会 EINVAL）；
- 通知采用 1 Hz 轮询 + 快照 seq 校验，未用信号量；如需事件驱动可再评估；
- 编译期断言：结构体 <1MB、magic 在偏移 0、seq 为 32 位且 4 字节对齐；
- 采集低开销：daemon 每类采集独立线程 + 单发布线程；GUI 常驻采集线程，
  GTK 主线程只渲染。

刷新路径性能（2026-11，压测后优化）：
- 慢点排查：FMON_REFRESH_STAGE=1 输出各阶段/单函数耗时（CALL_TIME），
  FMON_REFRESH_STATS=1 每分钟输出 p50/p95/p99；
- 已修：disk_queue 的 iostat fork/exec 移到 GTask 工作线程，不再阻塞主线程；
- 已修：ZFS 池详情、内存队列、CPU 时间分布、GPU 页等仅在其页面可见时更新，
  隐藏页不做格式化与布局；
- 效果：空闲 refresh p50 0.35ms / p95 0.51ms / p99 3.97ms
  （此前 p95 16.4ms / p99 24.3ms），16 线程满载 p95 6ms。

并发规范（GTK 单线程模型，必须遵守）：
- UI 只在主线程改；工作线程只做数据读取/解析，结果通过 GTask 回调
  （主上下文）或 g_idle_add/g_main_context_invoke 派发；
- 大命令（zpool iostat/status、vmstat -z、iostat -x、ifconfig）；磁盘温度已改为 NVMe ioctl/CAM，不再调用 nvmecontrol
  一律 g_subprocess_*_async，回调里只解析小文本，不阻塞主循环；
- 共享内存读写走 seqlock：seq 为 _Atomic uint32_t（<stdatomic.h>），
  写者 relaxed 置奇数 + release fence → 写数据 → release fence + relaxed
  置偶数；读者 acquire 读 seq、拷贝、acquire fence 后复读，重试上限 64；
- 逐线程临时缓冲区暂不需要（采集都在主线程或 GTask 工作线程内部）；
  以后若加共享 scratch，用 _Thread_local；
- 禁止在工作线程里调用任何 gtk_* 函数；新增采集如可能变慢，优先异步；
- 子进程统一用 posix_spawn（fmon_child.c），不再 fork+exec；
- 主循环慢调用看门狗：单次 >5 ms 打 g_warning（60 秒最多一条）；
  net_stats 已改为 GTask 工作线程采集（getifaddrs/read_tcp 在后台，
  无线缓存合并在主线程完成后发布），看门狗不再报它；
  disk_perf（devstat）也已改为 GTask 工作线程，主线程只发布结果；
  看门狗里偶尔还剩 refresh(total) 27~38 ms（UI 布局/标签刷新），
  不是采集慢，后续可用 GTK profiler 再优化。

面板数据统一规则（所有详情面板必须遵守）：键定宽 → 值 ELLIPSIZE_END → 列定宽；
不得让数据参与宽度谈判（避免刷新时列漂移），字号不变；共享实现
gui_fixed_row(key, value, key_width, max_chars)（support/gui_widgets.c）。

要求：C11 + 0 warning；先跑 ninja -C build 与 meson test -C build 建立基线；
每次只改一个模块，改完跑对应页面冒烟（--page proc/svc/gpu0/mem/netm/disk）。
若改 fmon_shm.h 必须升 ABI 并重装两端。
优先处理 HANDOFF 第 6 节“已知限制/技术债”。
```
