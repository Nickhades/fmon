# fmon —— FreeBSD 监控中心 / 资源管理器

用 C11 + GTK4 写的 FreeBSD 系统监控与资源管理工具，采用「特权守护进程采集 + 非特权图形界面显示」的结构：

```
        ┌──────────────────────┐        POSIX 共享内存（seqlock 单向发布）
        │  fmon-daemon (root)  │  ────────────────────────────────────────┐
        │  CPU/内存/磁盘/网络  │    /fmon_daemon_shm_v16  0644 只读映射    │
        │  ZFS/进程/挂载点     │                                          ▼
        └──────────────────────┘                              ┌────────────────────┐
                                                              │  fmon-gui (普通用户)│
                                                              │  GTK4 + Cairo 曲线  │
                                                              └────────────────────┘
```

* **守护进程**：1 Hz 采集，写共享内存。只写不读，界面崩溃不影响采集。
* **界面**：只读映射，每秒拷贝一份一致快照再渲染，永远看不到“半新半旧”的数据。
* **特权操作**（结束进程、启停服务）：界面通过 `pkexec` 以 argv 方式调用，**不经过 shell**。

---

## 1. 功能

### 监控

| 页面 | 内容 |
|---|---|
| 系统监控 | CPU（含每核柱状图）、物理内存、Swap、网络吞吐、GPU、磁盘吞吐 + 历史曲线；Top 进程 |
| ZFS 存储 | **多 zpool 支持**（卡片显示首个池，超过 1 个池时页面下方出现池列表：容量/健康/使用率/读写）；ARC 大小与上限、命中率、L2 命中、幽灵命中、读写带宽、ARC Prune |
| 硬件与电源 | CPU 温度（多传感器取最大值）、电池电量/充放电/交流电状态 |
| GPU | **智能探测所有显卡**：DRM 节点 + `hw.nvidia` + `pciconf`（驱动未加载也能识别型号）；卡片右上角下拉框切换；自动选中第一张可读负载的卡；显示负载、显存、温度、核心频率、PCI 槽位 |
| 存储空间 | 所有文件系统挂载点：设备、类型、总量/已用/可用、使用率进度条 |
| 网络连接 | TCP/UDP（IPv4+IPv6）连接列表，可按进程/PID/地址/协议过滤；网卡接口页（地址、MTU、速率、错误） |
| 系统日志 | `/var/log/messages` 等预设 + 任意文件，级别过滤、关键字搜索、暂停/跟随 |

### 资源管理

| 页面 | 能力 |
|---|---|
| 进程管理 | 列头排序、3 秒自动刷新（选中行时自动暂停）、搜索、CSV 导出；右键菜单：详情、打开的文件（lsof）、网络连接（sockstat，按 PID 过滤）、复制 PID、暂停（SIGSTOP）、恢复（SIGCONT）、降低优先级（nice 10）、结束（SIGTERM）、强制结束（SIGKILL） |
| 服务管理 | rc.d 服务列表、运行/停止分页、开机自启状态、搜索、隐藏系统内部脚本；详情、启动、停止、自启、禁用（停止/禁用需确认） |

### GPU 自动探测与切换

| 显卡 | 身份来源 | 负载来源 |
|---|---|---|
| NVIDIA | `hw.nvidia.gpus.N.model` → `pciconf -lv` | `nvidia-smi`（NVML）：负载、显存、温度、频率 |
| AMD（核显/独显） | `dev.drm.N.PCI_ID` + `pciconf` 型号 + 内置型号表 | `libdrm_amdgpu` 的 `AMDGPU_INFO_SENSOR`：负载、温度、频率；`AMDGPU_INFO_MEMORY`：VRAM / GTT |
| Intel | `pciconf` / DRM | FreeBSD 暂无接口 → 显示设备信息并标注“无传感器” |
| 未加载驱动的卡 | `pciconf -lv`（PCI 厂商数据库里的型号名） | 无 → 标注“驱动未加载” |

* 每 30 秒重新枚举，驱动后加载或热插拔都能自动发现。
* 优先展示第一张**能读到真实负载**的显卡；下拉框里带“（无传感器）/（驱动未加载）”后缀。
* 悬浮卡片可看到厂商、PCI 地址与槽位、传感器类型、显存/GTT、频率。
* **绝不用随机数伪造 GPU 数据**：读不到就明确显示 N/A。

### 其它

* **控制通道**：`fmon-daemon` 监听 `/var/run/fmon-ctl.sock`（0660，属组 `operator`），
  GUI 的「结束进程 / 暂停 / 恢复 / 优先级 / 启停服务 / 开机自启」通过它执行，
  **不再调用 pkexec**；守护进程侧对 pid、信号、服务名、nice 值逐一校验。
  守护进程未运行时自动回退到原来的 pkexec 流程。
* 守护进程离线 / 数据过期横幅提示，不会把“采集停了”误认为“系统空闲”。
* **阈值告警**：CPU / 内存 / Swap / ZFS 池 / 温度 / 低电量，越限时右上角显示 ⚠ 并通过 `notify-send` 发桌面通知（同一指标 10 分钟冷却）；阈值保存在 `~/.config/fmon/alerts.ini`。
* **资源限制（rctl）**：规则列表与添加/删除都通过控制通道（root）执行；
  需内核启用 `kern.racct.enable=1`。页面会区分「未启用」「需要 root 权限」「执行失败」三种状态，
  不会再因为普通用户读不到规则而误报“未启用”。
* **cpuset 绑定**：进程右键「CPU 绑定…」，查看当前 mask 并修改；自己的进程直接调用
  `cpuset -l`，其他用户的进程走控制通道（root）。
* **告警分级与历史**：超过阈值 5 个单位记为「严重」，否则「警告」；每次触发写入
  `~/.local/state/fmon/alerts.log`（2 MiB 自动轮转），侧边栏「告警历史」页面可查看最近 300 条，
  并支持 **静默 1 小时**（只停通知、仍记录）和清空历史。
* **细分明细**：网卡速率在「网络连接 → 网络接口」，磁盘速率在「存储空间 → 磁盘设备」
  （设备/读/写/读次/写次/繁忙），数据分别来自 `getifaddrs(2)` 与 `devstat(3)`。
* **进程内存列**按实际占用显示（KB/MB/GB 自动单位，取 RSS），不再显示百分比；
  排序也按实际字节数。导出 CSV 同时包含 `MEM(RSS KB)` 与 `MEM(%)` 两列。
* **历史记录**：守护进程每 10 秒把关键指标追加到 `history.tsv`
  （root 为 `/var/db/fmon/history.tsv`，普通用户调试为 `~/.local/state/fmon/history.tsv`，
  20 MiB 轮转）；界面「历史记录」页支持 **最近 1 小时 / 24 小时** 的 CPU 与内存曲线。
* 历史数据 CSV 导出（locale 安全，含网络发送与磁盘写入列）。
* **Windows 11 风格视觉**：强调蓝 `#0078d4` 选中态（左侧 3px 指示条）、扁平圆角卡片（8px）、
  平铺按钮、底部操作栏（进程页「结束任务 / 详细信息」）、下划线式标签页。
* **网络分组**：子项直接是「活动连接」「网络接口」两页，不再在页面内套标签页。
* **两级导航（任务管理器风格）**：左列分组；启动时内容区全宽显示「CPU」性能页，
  **首次点击某个分组时二级列表从左侧平滑滑出**，并立即显示该组的第一个子页面；
  之后二级列表保持显示，点子项平滑切换；`--page` 会自动定位分组与子项。
* **主题**：标题栏下拉可选「跟随系统 / 浅色 / 深色」，选择持久化到
  `~/.config/fmon/theme`；也可用 `fmon-gui --theme light` 启动。
* **内存条**：插槽数、每条容量/类型/标称与实际频率/厂商（依赖 dmidecode，安装脚本会检测）。
* **CPU 明细**：插槽数、逻辑处理器、基准/当前频率、虚拟化状态。
* **GPU 明细**：驱动/VBIOS、显存、利用率细分、时钟、功耗、PCIe 速率为吞吐、BAR1、ECC、P-State。
* **硬件明细**：网络页显示每个接口的类型（以太网/Wi-Fi/虚拟）、连接状态、速率、MAC、
  IPv4/IPv6、SSID、信道、信号强度与网卡芯片型号；磁盘页显示型号、序列号、容量、SSD/HDD。
* **多显卡**：「概览」子列表下每张 GPU 一个独立页面（自动命名、各自曲线与详情，主导航不单独分组）；
* **多显卡切换**：旧 GPU 页仍保留下拉框（`--page gpu`）；
  NVIDIA 遥测依赖 `nvidia-smi`：先查 PATH，再查常见目录
  （`/usr/local/bin`、`/usr/bin`、`/usr/local/nvidia/bin` 等），都不在时可用
  `FMON_NVIDIA_SMI=/你的路径/nvidia-smi` 指定——自编译、pkg 安装、自定义目录都适用。
* **更新时间**：顶部系统信息行附带「更新 HH:MM:SS」。
* **刷新节奏**：界面与采样同频（250ms），磁盘读写详情取最近 1 秒均值，
  短促 I/O 爆发不会出现“曲线有尖峰、下面却显示 0”的情况。
* **卡片层次**：标题、副标题与曲线共用一张大卡片（圆角+描边+投影），下方详情卡独立成组；卡片内部再叠渐变面板与统一小方块网格纹理。
* **物理核心浮窗**：CPU 页底部显示「N 核 · M 线程」与 **每个物理核一个方块**（颜色=该核平均负载）；
  点「物理核心视图」弹出独立窗口，标题只显示「当前 N 线程」，**一个线程一个方块**（自动识别
  `kern.smp.cores` / `kern.smp.threads_per_core`），按物理核分组、智能排布；悬停显示线程号、
  所属物理核、使用率与同核线程；`fmon-gui --core-window` 可直接打开。
* **大页面性能视图**：概览（CPU=使用率+温度同框、内存、Swap、网络、磁盘、GPU、电池、历史）、存储（ZFS 总览=池空间+ARC+命中率+Prune 同框、ZFS 带宽=读写同框、存储空间）全部为独立大页面：
  上部为合并图表：多条曲线各占一条带边框的横带（颜色区分、曲线下方渐变填充、各自量程、左侧动态上下限刻度、带名与当前值），页面标题下方显示 CPU/GPU 型号，共享时间轴，悬停显示同一时刻各条数值；下部为**横向分类卡片**（每类一张卡，卡片有表头，数值 16px 大字号），每秒同步刷新。内存的「可用」采用 FreeBSD 口径：`free + inactive + cache`，
  使用率 = (总量 − 可用) / 总量。
* `fmon-gui --page cpu|mem|swap|netm|disk|gpu|history|zpool|arc|zhit|zread|zwrite|zprune|temp|batt|storage|proc|svc|conn|iface|rctl|alerts|log`
  直接打开指定页面。
* `Ctrl+Q` 退出，`F5` 立即刷新。
* FreeBSD rc.d 启动脚本。

---

## 2. 安装

### 2.1 一键安装（推荐）

```sh
sudo ./install.sh
```

脚本会自动完成：环境与依赖检查（缺 `gtk4`/`cairo`/`meson`/`ninja` 会用 pkg 装好）→
编译（0 warning 才算通过）→ 停止旧的 fmon 进程 → 安装二进制与 rc.d 脚本 →
`sysrc fmon_daemon_enable=YES` → 启动服务 → 打印版本、共享内存与显卡探测结果。

常用选项：

| 选项 | 作用 |
|---|---|
| `-y, --yes` | 不询问，适合自动化 |
| `--dry-run` | 只打印步骤，不修改系统（装前先看一眼） |
| `--clean` | 删除 `build/` 后全量重编译 |
| `--build-only` | 只编译不安装（普通用户可用） |
| `--no-start` | 安装后不启动服务 |
| `--launch` | 安装后以桌面用户启动界面（需 `sudo -E` 保留 DISPLAY） |
| `--skip-deps` | 不自动装依赖，缺依赖直接报错 |
| `--uninstall` | 卸载：停服务 + 取消自启 + 删除文件 + 清理共享内存 |
| `--keep-data` | 卸载时保留共享内存与锁文件 |
| `--keep-legacy` | 不备份/移除旧版 `/usr/local/bin/fmon_daemon` |

```sh
./install.sh --dry-run -y     # 预演
sudo ./install.sh -y          # 无人值守安装
sudo ./install.sh --uninstall # 卸载
```

安装过程会记录到 `/var/log/fmon-install.log`。

> 旧版（`v9` 共享内存）如果还在运行，脚本会先停掉它并把
> `/usr/local/bin/fmon_daemon` 备份为 `.bak`，避免两个守护进程同时采集。

### 2.2 手动构建

依赖：

```sh
sudo pkg install meson ninja pkgconf gtk4 cairo
sudo pkg install libdrm        # 可选：AMD 显卡真实负载（libdrm_amdgpu）
```

`libdrm` 是**可选**依赖：构建时自动探测，找不到时守护进程仍会列出 AMD 显卡，
只是标记“无传感器”。

构建：

```sh
meson setup build
ninja -C build
meson test -C build        # 单元测试：fmon_child / fmon_shm
ninja -C build fmon-pot    # 更新 po/fmon.pot（翻译模板）
```

产物：

* `build/src/daemon/fmon-daemon`
* `build/src/gui/fmon-gui`

编译选项已包含：`-Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wpointer-arith -Wwrite-strings -Wvla`、
`-D_FORTIFY_SOURCE=2 -fstack-protector-strong`，链接 `-Wl,-z,relro,-z,now,-z,noexecstack`。
**当前代码在 clang 19 下 0 warning。**

也保留了 BSD make 的快捷构建（只用于快速试验，正式构建请用 meson）：

```sh
make            # 依赖 pkg-config 能找到 gtk4 / cairo
```

---

## 3. 运行

```sh
# 守护进程需要 root（kvm、部分 sysctl、/var/run 下的锁文件）
sudo ./build/src/daemon/fmon-daemon

# 另开终端，普通用户即可
./build/src/gui/fmon-gui
```

常用参数：

```sh
./build/src/gui/fmon-gui --page zfs    # 启动即打开 ZFS 页面
./build/src/gui/fmon-gui --gpu 1       # 多卡机器上默认显示第 2 张显卡
sudo ./build/src/daemon/fmon-daemon -f # 强制接管（清理残留实例时使用）
```

守护进程日志默认写 stderr；当 stderr 不是终端时（例如由 rc.d 拉起）自动改投 `syslog`，
可在 `/var/log/messages` 里看到 `fmon-daemon` 的记录。

---

## 4. 安装为系统服务

```sh
sudo ninja -C build install          # /usr/local/{sbin,bin} + /usr/local/etc/rc.d/fmon_daemon
sudo sysrc fmon_daemon_enable=YES
sudo service fmon_daemon start
```

`rc.d/fmon_daemon` 使用 `daemon(8)` 托管，异常退出可由 rc 体系重启；
日志写在 `/var/log/fmon-daemon.log`（可用 `fmon_daemon_logfile` 变量修改）。

---

## 5. 数据契约（共享内存）

```c
typedef struct {
    uint32_t magic;        /* 'FMON' */
    uint32_t abi_version;  /* 14 */
    uint32_t struct_size;  /* sizeof(FmonShmData) */
    uint32_t seq;          /* seqlock：奇数表示写入中 */
    int32_t  daemon_pid;
    int32_t  status;       /* INIT / RUNNING / STOPPED */
    uint64_t last_update_us;   /* CLOCK_MONOTONIC，微秒 */
    ...
} FmonShmData;
```

* **读者**（界面）先 `fstat` 校验大小，再校验 `magic/abi/size`，然后才允许访问字段；
  每次刷新用 `fmon_shm_snapshot()` 拷贝一致快照。
* **写者**（守护进程）每个写事务把 `seq` 自增两次（奇数→偶数），并用互斥锁串行化。
* **心跳**：`last_update_us` 每秒更新；界面超过 5 秒未更新即显示“数据已过期”。
* **单实例**：`/var/run/fmon-daemon.lock`（普通用户回退到 `/tmp`）+ 共享内存中的 `daemon_pid`
  存活探测。**注意：POSIX 共享内存对象不支持 `flock(2)`**，所以锁文件与共享内存是分开的。

修改 `FmonShmData` 布局时，必须递增 `FMON_ABI_VERSION` 并同时重编两端；不兼容时界面会给出明确提示而不是崩溃。

---

## 6. 安全模型

| 项 | 做法 |
|---|---|
| 共享内存 | 守护进程创建为 `0644`（任何人可读、仅 root 可写），界面只读映射 |
| 命令执行 | `fork + execvp`，argv 传参，**永不使用 `/bin/sh -c`**；输出上限 + 超时 |
| 特权操作 | `pkexec <绝对路径> <参数…>`，二次确认；本用户进程直接 `kill(2)` 不提权 |
| 输入校验 | 服务名只允许 `[A-Za-z0-9._+-]` 且必须存在于 rc.d 目录；PID 必须是正整数 |
| 计数校验 | 界面钳制 `gpu_count / proc_count / mount_count / iface_count` 等上报值 |
| 编译加固 | FORTIFY、stack-protector、PIE、RELRO/NOW、不可执行栈 |

> 本地其他用户仍然可以**读取**共享内存中的主机名、进程名、挂载点等信息（与 `ps`、`df` 的可见性一致）。
> 如果需要更严格，可把权限改为 `0640 root:operator` 并把桌面用户加入 `operator` 组，
> 同时修改 `src/common/fmon_shm.c` 里的 `tune_permissions()`。

---

## 7. 目录结构

> 细化到“每个功能一个模块”的清单见 [`docs/MODULES.md`](docs/MODULES.md)；
> 编译告警、命名、头文件保护等 C11 审核结论见 [`docs/CODE_AUDIT.md`](docs/CODE_AUDIT.md)；
> 后续维护/调试/分发交接见 [`docs/HANDOFF_NEXT_SESSION.md`](docs/HANDOFF_NEXT_SESSION.md)。

```
fmon4/
├── install.sh                   # 一键安装 / 卸载脚本
├── meson.build                  # 顶层构建（i18n、rc.d、许可证安装）
├── Makefile                     # 早期快速试验用（正式构建请用 meson）
├── README.md
├── CODE_REVIEW.md               # 代码审核与改造报告（含全部问题清单）
├── 测试.txt                     # 构建/运行/排错速查
├── docs/                        # 截图、MODULES.md、CODE_AUDIT.md、拆分交接
├── po/                          # gettext 翻译（fmon.pot 等）
├── rc.d/
│   └── fmon_daemon.in           # rc.d 模板（@PREFIX@ 由 meson 替换）
├── tests/
│   ├── test_child.c             # 子进程执行与超时
│   └── test_shm.c               # 共享内存 seqlock
└── src/
    ├── common/                  # 两端共享
    │   ├── fmon_shm.h/.c        # 共享内存 ABI、seqlock、快照、校验
    │   └── fmon_child.h/.c      # 无 shell 的子进程执行（超时 + 限流）
    ├── daemon/
    │   ├── core/                # 入口、控制通道、历史落盘、sysctl 辅助
    │   └── collectors/          # cpu / io / net / storage / zfs / gpu / meminfo / proc
    └── gui/
        ├── app/                 # 启动、窗口外壳、导航、样式、快照、导出
        ├── charts/              # 单曲线、分带曲线、几何与绘制
        ├── pages/               # CPU/GPU/内存/网络/磁盘页面与刷新调度
        ├── support/             # 控件、工具、主题、告警、控制通道、对话框
        ├── assets/              # 内置 SVG 图标（编进二进制，不依赖系统图标主题）
        └── views/               # 各功能列表视图
            ├── process_internal.h   # 进程页内部类型
            ├── process_model.c      # ps 解析、过滤、排序
            ├── process_actions.c    # 信号 / renice、右键菜单
            ├── process_windows.c    # 详情、lsof / sockstat 窗口
            ├── process_list.c       # 模型栈与 ColumnView
            ├── process_view.c       # 进程页装配（gui_process_view_new）
            ├── service_internal.h   # 服务页内部类型
            ├── service_model.c      # rc.conf 解析、rc.d 扫描
            ├── service_actions.c    # 启停 / 自启、服务详情
            ├── service_list.c       # 行构建、列表重建、过滤
            ├── service_view.c       # 服务页装配（gui_service_view_new）
            └── ...                  # storage / network / log / alert / rctl / history
```

---

## 8. 排错

| 现象 | 处理 |
|---|---|
| 界面顶部提示“未连接到 fmon-daemon” | 守护进程没启动，或共享内存权限不足；`ps aux \| grep fmon-daemon`，`sudo service fmon_daemon start` |
| 提示“ABI 不兼容” | daemon 与 gui 不是同一次构建：重新 `ninja -C build` 并**同时重启两者** |
| 提示“已有另一个 fmon-daemon 在运行” | 先 `sudo service fmon_daemon restart`；确认无残留进程后可用 `-f` 强制接管 |
| 温度/GPU/电池显示 N/A | 该机型没有对应传感器或驱动（`dev.cpu.N.temperature`、`nvidia-smi`、`libdrm_amdgpu`） |
| GPU 下拉框只显示一张卡 | 另一张卡缺少驱动（如 NVIDIA 未装 nvidia-kmod、AMD 未装 drm-kmod）或没有 `/dev/dri` 节点 |
| 核显显示“无传感器” | 未安装 `libdrm`，或守护进程没有 `/dev/dri/renderD*` 读权限（应作为 root 运行） |
| Swap 一直 N/A | 系统未配置 swap |
| 服务列表为空 | 勾选了“隐藏系统内部脚本”且没有第三方服务；取消勾选即可看到全部 |

调试技巧：

```sh
# 让 GLib 关键告警直接崩溃，便于定位
G_DEBUG=fatal-criticals ./build/src/gui/fmon-gui
# 拿到崩溃栈
lldb -c fmon-gui.core ./build/src/gui/fmon-gui -o 'bt' -o quit
# 查看采集源数据
sysctl kern.cp_time; swapinfo -k; zpool list -Hp
```

---

## 9. 路径与可移植性

- **基础系统路径直接使用**：`/bin`、`/sbin`、`/usr/bin`、`/usr/sbin`、`/etc`、`/var`
  在 FreeBSD 15+ 固定不变，程序直接调用（如 `/bin/kill`、`/usr/sbin/service`）。
- **安装前缀不写死**：`meson --prefix`（默认 `/usr/local`）决定二进制、rc.d 脚本、
  许可证与语言包位置；`install.sh` 支持 `--prefix=PATH`，rc.d 脚本里的路径由 meson
  在配置阶段替换。
- **运行时自适应查找**：外部工具按 `PATH → 安装前缀的 bin/sbin → 常用目录` 搜索；
  `nvidia-smi`/`nvtop` 额外支持 `FMON_NVIDIA_SMI` 覆盖；`pkexec` 同样按前缀与
  `/usr/bin`、`/bin` 依次查找。
- **服务发现**：同时扫描 `/etc/rc.d` 与 `<前缀>/etc/rc.d`、`/etc/rc.conf.d` 与
  `<前缀>/etc/rc.conf.d`，不依赖单一目录。

## 10. 权限与认证

系统级操作（结束进程、调整优先级、CPU 绑定、rctl 资源限制、服务启停、开机自启）
按以下顺序执行：

1. **控制通道**：`fmon-daemon` 以 root 运行，直接执行（无需密码）；
2. **pkexec**：控制通道不可用或被拒绝时，自动回退到 pkexec，由桌面环境的
   polkit 认证窗口提示输入管理员密码；
3. **直接调用**：只涉及自己的进程时（kill / renice / cpuset）不需要任何授权。

pkexec 使用解析出的绝对路径（`/usr/local/bin/pkexec` 等），不依赖桌面启动时的
`PATH`；界面会在执行前提示“需要管理员密码…”，失败原因会显示在对话框或错误框中。

---

## 10. 已知限制

* 只展示第一个 zpool（多池场景可扩展）。
* 界面：全局粗体、固定两列（左 3 行/右 3 行）、默认窗口 1500×920；
  **不记忆**窗口状态与页面，每次启动都从「系统监控」开始。
* 曲线：采样间隔 250 ms（1 Hz），历史 1200 点（5 分钟），每点带微秒时间戳；
  绘制使用单调三次插值（Fritsch–Carlson），数学上不会画出不存在的峰谷；
  鼠标悬停显示该点数值和相对时间（毫秒级）。
* 进程列表：普通用户视图只取自己的进程（约 30 行），并以数据指纹跳过无变化的重建，
  实测 `proc` 页面 CPU 已从 4.2% 降到 1.7% 左右；以 root 运行的全量视图仍有上千行。
* 界面文案已接入 gettext（`po/` + `ninja -C build fmon-pot` 生成模板，
  当前 123 条 msgid）；未附翻译，运行时回退为中文原文。
* 单元测试覆盖 `fmon_child` 与共享内存 seqlock（`meson test`），
  解析器（ps / sockstat / zpool）的测试仍待补充。
* 无 GPU 的虚拟机建议用 `GSK_RENDERER=cairo` 降低显存占用，但该渲染器在 GTK 4.20 上退出阶段存在 GTK 内部崩溃（与本项目代码无关），因此未强制启用。
* Intel 显卡目前只有设备信息，没有负载读数（FreeBSD 未暴露 `i915` 传感器）。
* `--page` / `--gpu` 只在**启动新实例**时生效；窗口已打开时请用侧边栏和 GPU 下拉框切换（GTK 单实例机制会忽略第二次启动的参数）。

更多问题清单、修复说明与后续建议见 **[CODE_REVIEW.md](CODE_REVIEW.md)**。

## 许可证

本项目使用 **BSD 2-Clause License**（版权人：Zhang Xin Yu），完整文本见 [LICENSE](LICENSE)。

- 界面右上角「关于」→「许可证：BSD 2-Clause」可随时查看完整协议；
- 安装后许可证文件位于 `/usr/local/share/licenses/fmon/LICENSE`；
- 二次分发（源码或二进制）时请保留版权声明与免责声明；
- 第三方组件（GTK/GLib/Pango/cairo/libdrm 等）的许可证汇总见
  [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md)，界面「关于 → 第三方组件」里也能查看。

## 字体渲染

界面文字不依赖桌面环境的 fontconfig 配置：

- 启动时强制 `GSK_RENDERER=cairo`（CPU 渲染，FreeType 光栅化）；
- 向 fontconfig 注入固定参数：抗锯齿、RGB 次像素、`hintslight`、`lcdfilter`，
  关闭字体自带点阵图；
- 字体族显式指定为 `Noto Sans → Noto Sans CJK SC → DejaVu Sans → Sans`，
  避免系统把 `Sans` 映射到点阵感较强的备用字体。

因此以 root 启动（不会读取桌面用户的字体配置）时，文字观感与普通用户一致。
