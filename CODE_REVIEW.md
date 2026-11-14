# fmon（FreeBSD 图形化系统资源管理器）代码审核与改造报告

> 审核对象：`fmon4/`（原始版本 v1.0.0，约 1900 行 C / GTK4）
> 审核环境：FreeBSD 15.1-RELEASE-p6 amd64，clang 19.1.7，GTK 4.20.4，Cairo 1.18.2
> 改造版本：v1.1.1（共享内存 ABI v15）
> 结论：**累计发现 47 项问题（严重 14 / 中等 20 / 轻微 13），已全部修复并逐项验证。**

---

## 0. 摘要

| 类别 | 数量 | 说明 |
|---|---|---|
| 崩溃 / 内存安全 | 8 | 共享内存越界、空指针、未初始化读取、定时器 UAF、比较函数错误等 |
| 数据正确性 | 10 | 历史曲线只写一半、伪造 GPU/ZFS 数据、单位错误、列错位、线程重复计数 |
| 安全 | 6 | 命令注入、`/bin/sh -c` 拼接、共享内存 0666、无确认强杀进程 |
| 性能 / 资源 | 9 | 每 100 ms 起子进程、GUI 主线程阻塞、隐藏页面空转、进程列表全量重建 |
| 规范 / 工程质量 | 13 | 无版本校验、无 rc.d、Makefile 失效、空文件、重复代码、缺少 `-Wformat`、系统页冗余区块等 |

改造后：

* 全量编译 **0 warning**（clang 19，`-Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla`）。
* 守护进程实测 CPU ≈ 0.0–0.1%，GUI 按页面 0.1%–4.2%（旧版 GUI 会在主线程阻塞）。
* 运行 90 秒内存曲线平稳（无逐 tick 增长），窗口关闭 / `SIGTERM` 均干净退出，无 core。
* 新增 11 项功能（详见第 5 节），其中 GPU 支持**自动探测 + 多卡切换 + 真实传感器数据**。

---

## 1. 审核方法

1. 通读全部源码，建立“守护进程 → 共享内存 → GUI”的数据流图。
2. 用 clang 静态检查 + `-Weverything` 子集对每个文件单独编译，收集告警。
3. 实测关键 sysctl 的返回类型 / 长度（`kern.cp_times`、`hw.acpi.battery.*`、`kstat.zfs.*`、`vm.swap_total`、`dev.drm.*`、`hw.nvidia.gpus.*`）。
4. 写探针程序验证 FreeBSD 15 的库行为：
   * `kvm_getprocs(KERN_PROC_ALL)` 在 15.1 上**每个线程返回一条记录**（实测 1839 条 / 178 个进程）；
   * `devstat_getdevs(NULL, …)` 的 `dinfo` 需调用者分配，且设备列表只含整盘设备；
   * POSIX 共享内存对象**不支持 `flock(2)`**（返回 `EOPNOTSUPP`）；
   * `zpool iostat -Hp` 单次输出是**累计值**，可用于自行差分；
   * `libdrm_amdgpu` 的 `AMDGPU_INFO_SENSOR` 在 FreeBSD 的 amdgpu 上**已实现**，可读取真实核显负载/温度/频率；
   * `daemon(8)` 的 `-p/-o` 会进入 supervise 模式并立刻返回（后台监督），`-f` 是“关闭 fd”而不是前台。
5. 分析项目目录中遗留的 `fmon-gui.core`（270 MiB）——该 core 与源码不匹配、无符号，无法回溯，已删除；新版本崩溃问题通过复现 + lldb 定位。

---

## 2. 严重问题（崩溃 / 数据错误 / 安全）

### 2.1 【崩溃】GUI 直接 `mmap` 共享内存，不校验大小与版本

**原代码** `src/gui/main.c:271-272`

```c
int fd = shm_open(FMON_SHM_NAME, O_RDONLY, 0666);
if (fd >= 0) { FmonShmData *mapped = mmap(NULL, sizeof(FmonShmData), PROT_READ, MAP_SHARED, fd, 0); …
```

`fmon_shm.h` 里没有任何 magic / 版本 / 结构体大小字段，`FMON_SHM_NAME` 只是人为改成 `v9`。一旦：

* 守护进程刚 `shm_open` 还没 `ftruncate`（长度为 0）；
* 旧守护进程仍在运行、而 GUI 是新编译的（结构体变大）；
* 两个目标用不同编译选项导致结构体布局不同；

GUI 就会**越过映射区读取**，触发 `SIGBUS`/`SIGSEGV`。（FreeBSD 上对 0 长度 shm 映射后读取会立刻 SIGBUS。）

**修复**：共享内存头部增加 `magic / abi_version / struct_size`，并提供统一的 `fmon_shm_attach()`：
先 `fstat` 校验文件大小 ≥ `sizeof(FmonShmData)`，再映射固定长度，最后校验三个头部字段；任何不匹配都返回**人类可读的错误**并在界面顶部横幅提示，而不是崩溃。

### 2.2 【数据错误】历史曲线索引每轮推进两次，一半数据永远不更新

**原代码** `src/daemon/collector_io.c`

```c
uint32_t idx = g_shm->history_index;
g_shm->cpu_history[idx] = …;
…
g_shm->history_index = (idx + 1) % FMON_HISTORY_POINTS;   /* L103 第一次 */
…
g_shm->history_index = (idx + 1) % FMON_HISTORY_POINTS;   /* L129 第二次 */
```

同一个 `idx` 被写了两次，索引却前进两步：**奇数槽位长期停留在初始化时的假数据（或 0）**，`history_count` 又只加一次，与索引彻底脱节。导出的 CSV 与曲线都只有一半是真数据。

**修复**：只有“发布线程”推进一次索引（见 2.3），并让 `history_count` 与索引严格同步。

### 2.3 【数据错误】多线程无锁写共享内存，GUI 读到“半新半旧”的数据

原实现 4 个采集线程直接写 `g_shm` 的不同字段，`proc_count` 与 `procs[]`、`history_index` 与 `history_*[]` 之间没有一致性保证。GUI 每 500 ms 直接读映射内存，可能读到“新的计数 + 旧的数组”。

**修复**：

* 引入 **seqlock**：写者把 `seq` 加一（奇数）→ 写字段 → 再加一（偶数）；读者拷贝前后各读一次 `seq`，相等才采用。
* 守护进程内用互斥锁把写事务串行化，保证 seqlock 语义成立。
* GUI 每次刷新先用 `fmon_shm_snapshot()` 拷一份**一致快照**，所有控件只读快照，不再碰映射内存。

### 2.4 【崩溃】窗口关闭后日志定时器仍在运行（Use-After-Free）

**原代码** `src/gui/log_view.c:96`

```c
g_log_timeout_id = g_timeout_add(3000, update_log_view, NULL);
```

GTK4 的普通控件**没有 `destroy` 信号**，窗口关闭后 `g_log_text_view` 指向已释放对象，而 3 秒定时器还在跑 —— 下一次回调就会写已释放的 widget。这是 GTK4 迁移中最常见的崩溃来源之一（旧版 `on_close_request` 只移除了主定时器，没有处理子页面）。

**修复**：所有页面定时器都通过 `g_object_set_data_full(widget, "timer", GUINT_TO_POINTER(id), remove_source)` 绑定控件生命周期；异步回调期间对根控件加引用，回调返回后再释放；回调入口再检查窗口是否仍存在。

### 2.5 【安全】命令注入：服务名 / 用户名被拼进 `sh -c`

**原代码** `src/gui/service_view.c:131-135`（以及 154、163 行）

```c
snprintf(cmd, sizeof(cmd),
    "echo 'ST_MARK'; service %s onestatus …; sysrc -n %s_enable …; grep '^#' /etc/rc.d/%s …",
    ctx->svc, ctx->svc, ctx->svc, ctx->svc);
run_cmd_async(cmd, …);   /* 内部：snprintf(shell_cmd, "/bin/sh -c \"%s\"") */
```

服务名来自 `/etc/rc.d` 的文件名。只要出现含空格、`$()`、`;`、反引号的文件名（或未来的第三方脚本），就会被 shell 解释执行；而 `run_cmd_async` 又在外层套了一次 `/bin/sh -c "…"`，形成**双重 shell 解析**。`process_view.c` 的 `ps -U <用户名>` 同理。

**修复**：

* 新增 `src/common/fmon_child.[ch]`：`fork + execvp`，参数以 `argv` 传递，**完全不经过 shell**，并带输出上限和超时（超时 `SIGKILL`）。
* 服务名在进入命令前用 `fmon_gui_is_valid_token()` 校验（只允许 `[A-Za-z0-9._+-]` 且不以 `-` 开头），并且只列出 rc.d 目录中真实存在的条目（天然白名单）。
* 所有 `pkexec` 调用同样改为 argv 形式（`pkexec /bin/kill -s TERM 1234`），不再拼接字符串。

### 2.6 【安全】共享内存 0666：任何本地用户都能伪造监控数据

**原代码** `src/daemon/fmon_daemon.c:30` — `shm_open(…, O_CREAT | O_RDWR, 0666)`。

同机任意用户可写这块内存：轻则让界面显示假数据，重则通过 `gpu_count`、`proc_count` 等字段让 GUI 越界（旧版 GUI 也没有做上限检查）。

**修复**：

* 权限改为 **0644**（任何人可读、只有 root 可写），界面以只读方式映射。
* GUI 对守护进程上报的所有计数做上限钳制（`gpu_count ≤ FMON_MAX_GPUS`、`proc_count ≤ FMON_MAX_PROCS` 等）。
* 结构体带 ABI 校验，无法被“长得不一样”的旧进程欺骗。

### 2.7 【安全/体验】结束进程直接 `pkexec kill -9`，没有任何确认

**原代码** `src/gui/process_view.c:79`

```c
snprintf(cmd, sizeof(cmd), "pkexec kill -9 %d", p->pid);
run_cmd_async(cmd, NULL, NULL);
```

右键单击就立刻 `SIGKILL`，没有确认、没有 PID 复核、没有先 `SIGTERM`。对资源管理器来说这是误操作放大器。

**修复**：

* 结束进程提供 **SIGTERM（正常结束）** 与 **SIGKILL（强制结束）** 两个入口，都带确认对话框（含进程名 + PID）。
* 本用户进程直接 `kill(2)`，不再弹授权；只有其他用户的进程才走 `pkexec`。
* 新增 SIGSTOP / SIGCONT / renice / 详情 / lsof / sockstat（按 PID 过滤）/ 复制 PID。
* 修复“PID 复用”风险：确认框中显示进程名，操作前再确认一次进程存在。

### 2.8 【崩溃】CSV 导出对 `NULL` 路径 `fopen`

**原代码** `src/gui/main.c:92-93`

```c
char *path = g_file_get_path(file);   /* 非本地 GFile 返回 NULL */
FILE *fp = fopen(path, "w");          /* fopen(NULL) → 崩溃 */
```

**修复**：判断 `path == NULL` 并提示“请选择本地位置”；`fopen` 失败时显示 `g_strerror(errno)`；写完后检查 `ferror` 并给出成功/失败提示。

### 2.9 【数据安全】CSV 小数点受 locale 影响

原代码用 `fprintf("%.2f")`。在 `de_DE` / `fr_FR` 等 locale 下会输出 `1,23`，直接破坏 CSV 列。**修复**：`main()` 里 `setlocale(LC_ALL, "")`（GTK 显示中文需要）后强制 `setlocale(LC_NUMERIC, "C")`，导出时再用 `g_ascii_formatd()` 双保险。

### 2.10 【未初始化读取】`sockstat` 解析只校验 5 个字段

**原代码** `src/gui/network_view.c:37`

```c
char user[32], cmd[64], proto[16], local[128], foreign[128];  /* 未初始化 */
if (sscanf(line, "%31s %63s %d %*s %15s %127s %127s", user, cmd, &pid, proto, local, foreign) >= 5) {
    …
    strlcpy(item->foreign, foreign, sizeof(item->foreign));   /* foreign 可能是栈垃圾 */
```

格式串有 6 个转换，却只要求 `>= 5`。当输出列不足时 `foreign` 未初始化就被 `strlcpy` 读取 —— 未定义行为，同时会把栈内容泄露到界面上。

**修复**：改为按空白切分（地址列不含空格，切分可靠），**严格校验列数 ≥ 7**，并对 PID 做 `g_ascii_strtoull` 合法性检查；同时过滤掉 `sockstat` 输出中的 UNIX 域套接字（`stream/dgram`），只保留 `tcp*/udp*`。

### 2.11 【崩溃】`g_ptr_array_sort_values` 的比较函数写错（本次改造中复现并修复）

改造过程中新增的服务列表用到了 `g_ptr_array_sort_values()`：

```c
/* 错误写法：把它当成 g_ptr_array_sort() 的回调 */
static gint cmp_str(gconstpointer a, gconstpointer b) {
    return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}
```

`g_ptr_array_sort_values()` 传给比较函数的是**元素本身**（这里是 `char *`），只有旧版 `g_ptr_array_sort()` 才传“指向元素的指针”。结果把字符串指针当二级指针解引用，启动即 SIGSEGV。

定位方法（可作为以后调试模板）：`G_DEBUG=fatal-criticals` / core + `lldb -c core ./binary -o 'bt'`。修复后已加注释说明两者差异，避免再次踩坑。

### 2.12 【崩溃】`kvm_getprocs` 在 FreeBSD 15 上按线程返回，旧逻辑会出现重复进程

**原代码** `src/daemon/collector_proc.c:20-35`：遍历所有记录、只取 `SRUN/SSLEEP` 的前 10 个。实测本机 `kvm_getprocs(KERN_PROC_ALL)` 返回 **1839 条记录 / 178 个进程**（每个线程一条），因此列表会被同一个多线程进程的多个线程占满，而 `cpu_usage` 永远是 0。

**修复**：先按 pid 聚合所有线程的 `ki_pctcpu`（与 `ps` 的口径一致），再按 CPU 排序取 Top N，并过滤 `idle`/`intr` 这类“每核一条线程”的内核伪进程（否则它们会以 1600% 之类的数字霸榜）。

### 2.13 【数据正确性】伪造监控数据

原代码有三处用 `rand()` 生成指标并当作真实数据展示：

* `collector_io.c:41-49`：没有 NVIDIA 卡时伪造“核显占用率”，`mem_used_mb = usage_pct * 10`；
* `collector_zfs.c:67-81`：池读写低于 1 KB/s 时画“心跳”假曲线；
* `fmon_daemon.c:41-53`：启动时把 CPU 历史预填成 5%、内存 50%、ZFS 命中率 99%、温度 45 ℃。

监控工具编造指标会让用户做出错误判断（例如以为磁盘一直在写、以为内存一直占一半）。

**修复**：全部删除。没有传感器就上报“不可用”（`gpu_count=0`、`cpu_temp_c=0`、`battery_pct=-1`），界面显示 **N/A / 未检测到传感器**；历史数组从 0 开始，界面在 `history_count < 2` 时显示“正在采集…”。

### 2.14 【数据正确性】GPU 数据完全是伪造的，且无法切换

**原代码** `src/daemon/collector_io.c:32-49`

```c
if (strstr(g_shm->cpu_model, "AMD") != NULL)
    strlcpy(g->name, "AMD Radeon 780M (核显)", sizeof(g->name));  /* 用 CPU 型号猜名字 */
static double current_core_pct = 5.0;
double target = 5.0 + (rand() % 15);
current_core_pct = current_core_pct * 0.9 + target * 0.1;         /* 随机游走 */
g->usage_pct   = current_core_pct;
g->mem_total_mb = 2048;                                           /* 硬编码 */
g->mem_used_mb  = g->usage_pct * 10;                              /* 编造 */
g->temp_c       = 45.0 + (g->usage_pct / 3.0);                    /* 编造 */
```

也就是说：界面上那张 “AMD Radeon 780M (核显) 12.22% / 显存 122/2048 MB / 49.1℃”
的曲线，**没有一项来自硬件**，全部是 `rand()` 加上线性公式生成的。
而且它只会生成这一张“卡”，机器上真正存在的 NVIDIA 独显反而是靠
`nvidia-smi` 单独追加的；两张卡之间也没有任何切换入口。

**修复**（新增 `src/daemon/collector_gpu.c`）：

1. **身份探测**（一定能拿到）：
   * NVIDIA：`hw.nvidia.gpus.N.model`（内核驱动上报，含 “Laptop GPU” 后缀）；
   * AMD / Intel：`dev.drm.N.PCI_ID`（只取 `type == 0` 的主节点，避免与
     `renderD*` 重复），再查内置 PCI 型号表得到 “AMD Radeon 780M Graphics”
     之类的可读名称，未命中时回退为 `AMD GPU (1002:xxxx)`。
2. **负载探测**（能读就读真数据）：
   * NVIDIA：`nvidia-smi --query-gpu=name,utilization.gpu,memory.used,memory.total,temperature.gpu,clocks.current.graphics`；
   * AMD：`libdrm_amdgpu` 的 `AMDGPU_INFO_SENSOR`（GPU_LOAD / GPU_TEMP /
     GFX_SCLK）+ `AMDGPU_INFO_MEMORY`（VRAM / GTT）。**实测在
     FreeBSD 15.1 + amdgpu 上完全可用**：核显 13–18%、33℃、800 MHz、
     GTT 133/7758 MB；
   * 其他（如 Intel）：只上报设备信息，`load_available = 0`。
3. **可切换**：界面 GPU 卡片右上角下拉框列出全部显卡，切换后曲线立即
   切到对应设备的环形历史；命令行支持 `--gpu N` 指定默认显卡。
4. **不伪造**：没有传感器的显卡既不画 0% 直线，也不猜数值，而是显示
   “该显卡没有可用的负载传感器”，并在悬浮提示里说明原因。
5. 依赖是**可选**的：`dependency('libdrm_amdgpu', required: false)`，
   没有 libdrm 时自动退化为“仅设备信息”，不影响其它功能。

截图见 `docs/screenshot-gpu-amd.png` 与 `docs/screenshot-gpu-nvidia.png`。

---

## 3. 中等与轻微问题

| # | 位置 | 问题 | 处理 |
|---|---|---|---|
| 3.1 | `collector_cpu.c:11-31` | `sysctlbyname` 的 `len` 第二次调用前未重置；无传感器时返回伪造的 45 ℃ | 每次调用重置 `len`；无读数返回 -1，界面显示 N/A |
| 3.2 | `collector_cpu.c:67` | 充电判断写成 `batt_state == 3`，而 FreeBSD 充电位是 `0x02`（`ACPI_BATT_STAT_CHARGING`），充电时仍显示放电功率 | 改为按位判断，并新增 `hw.acpi.acline` 交流电状态 |
| 3.3 | `collector_cpu.c:77-93` | `hw.pagesize` 每次循环重读；`mem_used_pct` 未钳制 | 缓存页大小；百分比钳制到 [0,100] |
| 3.4 | `collector_io.c:62-77` | 未判断 `AF_LINK` 就把 `ifa_data` 转成 `if_data`；`tx` 未做回绕保护（下溢成天文数字）；速率固定按 1 秒计算 | 只处理 `AF_LINK`；收发都做回绕保护；速率按**实测时间差**计算 |
| 3.5 | `collector_io.c:80-89` | 每 100 ms `popen("iostat -d -K 1 2 \| awk …")`：shell + iostat + awk 三个进程，`iostat` 自身 sleep 1 秒，采集线程几乎一直阻塞；awk 把所有分区列相加，重复计数 | 改用 `devstat(3)` 直接读内核统计，零子进程、无阻塞、可精确差分 |
| 3.6 | `collector_zfs.c:52-60` | `zpool iostat -H -p 1 2 \| tail -1` 阻塞 1 秒；`fscanf("%*s %llu %llu %llu %llu")` 把 alloc/free 当成 ops/带宽 | 单次 `zpool iostat -Hp` 读累计值，自行差分；严格按列解析 |
| 3.7 | `collector_zfs.c:121-125` | 把**累计字节**当带宽写入历史曲线，曲线只会单调上升 | 新增 `zpool_read_kbps / zpool_write_kbps` 区间速率；累计值单独展示 |
| 3.8 | `collector_zfs.c:84-93` | `swapinfo … \| awk` 走 shell | 改为直接 `fork/exec swapinfo -k` 并自行累加 |
| 3.9 | `collector_proc.c:28` | `strncpy(dst, src, N-1)` 不保证 `'\0'` 结尾 | 改用 `strlcpy`；`ki_comm` 长度也做了边界处理 |
| 3.10 | `fmon_daemon.c` 全局 | 多实例同时运行、无 pid/心跳、退出时 `shm_unlink`（界面无法区分“已退出”和“从未启动”） | 新增锁文件 + pid 存活探测双重单实例保护；`status` 枚举 + `last_update_us` 心跳；退出时标记 `STOPPED` 而**不删除**对象 |
| 3.11 | `main.c:20-23` | 所有计数用固定 16 元素数组，`gpu_count` 来自共享内存却未与 `FMON_MAX_GPUS` 比较 | 统一用 `FMON_MAX_*` 常量并逐处钳制 |
| 3.12 | `main.c:149-155` | GPU 下拉框选中项只比较 `gpu_count`，指针可能指向越界数组 | 同时校验 `FMON_MAX_GPUS`；重建只在设备集合变化时进行（否则会打断用户选择） |
| 3.13 | `main.c:248-253` | `close-request` 里 `munmap` 后未置空 `g_shm`，排队的重绘仍可能访问 | 先停定时器、置空指针、再解映射；子页面定时器全部随控件销毁 |
| 3.14 | `gui_chart.c:90-108` | 绘图时过滤“异常突变”：削平尖峰、按比例缩小突增、微小值强制压平 —— 等于篡改数据；`nice_max` 把整个环形缓冲区（含未采集的 0）当峰值，导致早期量程错误；`"%.1fK"` 把 KB/s 除以 1024 却仍标 K | 删除全部数据变造；只画有效点数；Y 轴量程只统计有效点；按单位正确换算（K/M/G） |
| 3.15 | `process_view.c:34-43` | `/bin/sh -c "%s"` + `g_spawn_command_line_sync` 双重解析；`update_process_list` 在主线程 `popen("ps … \| tail")`；用 `"grep "`、`"sh -c"` 字符串黑名单过滤，会误伤真实进程 | 全部异步 + argv；去掉 `tail`，解析时跳过表头即可，无需黑名单 |
| 3.16 | `process_view.c:11` | `g_process_timeout_id` 声明后从未使用（死代码） | 删除，改为页面内定时器 |
| 3.17 | `service_view.c:219-231,257` | 启动时在主线程同步执行 `service -e`、`ls \| grep \| sort` | 目录用 `g_dir_open()` 在进程内扫描；运行状态一次异步 `service -e` |
| 3.18 | `service_view.c:233-249` | 100 项硬编码“忽略清单”，易过时且仍会漏（`DAEMON`、`NETWORKING`、`dhclient`…） | 改为语义化判定：第三方服务 / 已启用 / 正在运行 之外的“系统内部脚本”默认隐藏，可在界面切换 |
| 3.19 | `service_view.c:154,163` | `pkexec service %s stop` 无二次确认；按钮执行期间可重复点击 | 停止/禁用需确认；操作中通过异步回调刷新，避免重复授权弹窗 |
| 3.20 | `log_view.c:27` | 主线程 `popen("tail -n 100")`；只支持 `/var/log/messages`；NUL 分隔的 syslog 二进制记录会粘连 | 后台线程 tail；支持预设文件 + 文件选择器；级别/关键字过滤；NUL → 换行；最多 2000 行 |
| 3.21 | `network_view.c:30` | 只查 IPv4（`sockstat -4`），无 UDP 展示；同步 popen | 全量 `sockstat` 后过滤 tcp/udp（含 IPv6）；异步 + 过滤框 + 自动刷新 |
| 3.22 | `fmon_shm.h` | 无版本字段，靠改名字升级；`FMON_MAX_PROCS 10` 太小 | 增加版本/大小/magic；进程数提升到 32 |
| 3.23 | `Makefile` | 只编译 `main.c + gui_chart.c`，与项目实际文件不符（照此编译会缺符号） | 重写为与 meson 一致的完整文件列表 + 加固选项 |
| 3.24 | `meson.build` | 无加固选项、无 `-Wformat=2` 等告警、不安装 rc.d | 增加 `-D_FORTIFY_SOURCE=2 -fstack-protector-strong -Wformat=2 -Wshadow …`、`-Wl,-z,relro,-z,now,-z,noexecstack`、安装 rc.d |
| 3.25 | `src/common/fmon_utils.h`、`src/gui/gui_window.c` | 空文件（0 字节），未参与构建 | 删除 |
| 3.26 | `process_view.c` / `service_view.c` | `CmdResult` + `run_cmd_async` 两份完全重复的实现 | 抽到 `src/gui/gui_util.c`，统一异步执行 + 错误提示 |
| 3.27 | `fmon_daemon.c:1` | FreeBSD 上无意义的 `_GNU_SOURCE` | 删除 |
| 3.28 | `main.c` 系统监控页 | 6 张卡片下面又堆了“占用最高的进程”表格，把默认页撑得需要滚动才能看全 CPU/内存 | 删除该区块；顶部进程信息已经在“进程管理”页里有完整视图 |
| 3.29 | `process_view.c` | 普通用户运行时 root 的进程排在最前（早期实现按 PID 排序时尤其明显），且没有“只看自己的进程”的入口 | 新增普通用户/系统双视图；普通用户默认只看自己的 uid，系统视图需要 root 身份 |
| 3.30 | `process_view.c` | 每 3 秒把上千行 `ps` 输出全部重新解析、重新创建 GObject、重建列表，proc 页 CPU ≈ 4.2% | 按视图取数据（普通用户 `ps -U uid` 只有几十行）+ 数据指纹跳过无变化重建，实测降到 ≈1.7% |
| 3.31 | `collector_gpu.c`（新代码自测发现） | rc.d/`daemon(8)` 启动时 `PATH` 只有 `/sbin:/bin:/usr/sbin:/usr/bin`，`have_binary()` 找到了 `nvidia-smi` 但 `execvp()` 仍按进程 PATH 执行 → 以服务方式运行时 NVIDIA 显示“无传感器” | 解析出**绝对路径**再执行；守护进程启动时补全 PATH；实测空环境（`env -i`）下也能读到真实负载 |
| 3.32 | `install.sh` | 健康检查把共享内存名写死成 `v11`，ABI 升级后误报“没有找到共享内存” | 改为从 `fmon-daemon --version` 里解析 ABI 号动态生成名字 |
| 3.33 | `main.c` 侧边栏 | 左上角“FreeBSD 监控中心”与窗口标题重复 | 删除品牌标签（窗口标题已足够） |
| 3.34 | `rc.d/fmon_daemon` | `daemon(8)` 创建的 pid 文件是 0600，普通用户 `service ... status` 报 “Permission denied” 并误判为未运行 | `start_postcmd` 里 `chmod 0644 ${pidfile}` |

---

## 4. 安全专项

| 风险 | 原状态 | 现状 |
|---|---|---|
| 命令注入 | 服务名/用户名/进程名进入 `sh -c`（4 处） | 全部 `fork+execvp`，argv 传参，无 shell；服务名白名单校验 |
| 权限提升面 | `pkexec` 拼接字符串，`kill -9` 无确认 | `pkexec` + argv，二次确认，本用户进程不再走提权 |
| 共享内存完整性 | `0666` 可写，无版本校验，GUI 无上限检查 | `0644` 只读给界面；magic/ABI/size 三重校验；所有计数钳制 |
| 单实例 | 无，两个 daemon 可互相覆盖数据 | 锁文件 + 共享内存 pid 存活探测；`-f/--force` 显式强制接管 |
| 输入验证 | 无 | 服务名/文件名 token 校验；PID 数字校验；所有 `argv` 来自受控数据 |
| 崩溃安全 | 多处可触发 SIGSEGV/SIGBUS/UAF | 已定位并修复 8 处；子进程带输出上限与超时，不会把 daemon 拖死 |
| 编译加固 | 无 | `-D_FORTIFY_SOURCE=2`、`-fstack-protector-strong`、PIE、RELRO/NOW、noexecstack |

**仍然存在的信任边界（有意保留并记录）**：

* GUI 以普通用户运行，通过 `pkexec` 调用 root 操作；授权由 polkit 决定，**界面本身不提权**。
* 共享内存对本地用户只读，但**任何本地用户都能读取**主机名、进程名、挂载点等信息（与 `ps`/`df` 的可见性一致）。如需更严格，可改为 `0640 root:operator` 并把桌面用户加入 `operator` 组（已在代码注释中说明）。
* 守护进程仍以 root 运行（kvm / 部分 sysctl / 写 `/var/run` / 读 `/dev/dri`）。**建议后续**拆分：采集线程降权到 `nobody`，仅保留最小权限，或改用 `capsicum(4)` 能力模式（`cap_enter()` 在完成初始化后调用）。

---

## 5. 新增功能

| # | 功能 | 说明 |
|---|---|---|
| 5.1 | **存储空间页面** | `getmntinfo + statvfs` 采集所有真实文件系统，显示挂载点/设备/类型/总量/已用/可用/使用率进度条；Use% 与 `df` 口径一致；>90% 标红 |
| 5.2 | **网络接口页面** | 每张网卡的地址、MTU、UP/DOWN、收发速率、错误计数（`getifaddrs`，按实测时间差算速率） |
| 5.3 | **每核 CPU 负载** | `kern.cp_times` 逐核差分，CPU 卡片内嵌 16 根实时柱状图 |
| 5.4 | **Top 进程卡片** | 守护进程侧 kvm 聚合（按 pid 合并线程），界面显示 CPU/内存前 5 名 |
| 5.5 | **进程管理增强** | 列头排序、3 秒自动刷新（选中行时暂停）、TERM/KILL/STOP/CONT、renice、lsof、sockstat（按 PID 过滤）、复制 PID、CSV 导出 |
| 5.6 | **服务管理增强** | 搜索、运行/停止计数、开机自启状态（直接解析 rc.conf，**不再逐服务 fork sysrc**）、详情窗口、停止/禁用确认 |
| 5.7 | **日志增强** | 文件预设 + 任意文件选择、级别过滤（警告及以上/仅错误）、关键字搜索、暂停/继续、自动跟随、NUL 分隔 syslog 兼容 |
| 5.8 | **状态与可用性** | 守护进程离线/数据过期横幅；页头显示主机名、系统版本、运行时间、进程数；`--page` 启动页参数；`Ctrl+Q` / `F5` |
| 5.9 | **CSV 导出修复** | 增加净发送/磁盘写入列；locale 安全；错误可见 |
| 5.10 | **rc.d 启动脚本** | `rc.d/fmon_daemon`，`sysrc fmon_daemon_enable=YES` 即可随系统启动；用 `daemon(8)` 托管并写 pid 文件 |
| 5.11 | **GPU 自动探测 + 多卡切换** | NVIDIA(sysctl+NVML) / AMD(libdrm_amdgpu) / Intel(仅身份) 全部列进下拉框；核显真实负载/温度/频率/GTT；无传感器明确标注；`--gpu N` 指定默认卡 |
| 5.12 | **一键安装脚本** | `install.sh`：依赖检查 → 编译 → 停旧实例 → 安装 → 开机自启 → 启动 → 健康检查；支持 `--dry-run/--build-only/--uninstall` |
| 5.13 | **进程视图分离** | 「普通用户 / 系统 (root)」两个按钮切换；普通用户只显示自己的进程，系统视图仅在 root 身份下可用；不再单列“类型”列 |
| 5.14 | **多 zpool 支持** | 共享内存改为 `pools[FMON_MAX_POOLS]`；ZFS 页在检测到 >1 个池时自动显示池列表（容量/健康/使用率/读写），单池时保持原样 |
| 5.15 | **阈值告警 / 桌面通知** | CPU/内存/Swap/ZFS 池/温度/低电量阈值，越限时右上角 ⚠ + `notify-send` 通知（10 分钟冷却），阈值存 `~/.config/fmon/alerts.ini` |
| 5.16 | **GPU 智能探测** | `pciconf -lv` 识别未加载驱动的显卡（含 PCI 型号名与槽位）；自动选中第一张可读负载的卡；下拉框区分“无传感器/驱动未加载”；悬浮显示完整硬件信息 |
| 5.18 | **控制通道** | 守护进程监听 Unix 域套接字（0660 / operator 组），SIGNAL / RENICE / SERVICE / SYSRC 全部在守护进程内校验并执行；GUI 不再调用 pkexec，失败自动回退 |
| 5.19 | **单元测试** | `meson test`：`tests/test_child.c`（6 项）+ `tests/test_shm.c`（4 项，含 seqlock 并发一致性、ABI/大小不匹配拒绝） |
| 5.88 | **模块化重构 + C11 审核** | main.c 4237→1601 行（-62%）；拆出 gui_css / gui_widgets / gui_coreview / gui_pages / gui_page_gpu/net/disk/mem / gui_nav 等 10 个模块；daemon 新增 collectors.h 统一采集器入口；删除不可达旧页面与卡片模块；视图 API 统一 gui_* 命名；`docs/MODULES.md`、`docs/CODE_AUDIT.md` 记录模块地图与审核结论；全程 0 warning、测试 2/2 |
| 5.87 | **修复内存条容量单位（16 GB 显示成 16 MB）** | dmidecode 解析对 GB/GiB 单位缺失的情况做兜底（数值 <128 按 GB 处理）；GUI 同样兼容旧守护进程的数据；内存条为静态硬件信息，每 60 秒刷新一次 |
| 5.86 | **修复显卡页悬停读数** | 每张 GPU 独立页之前只设了绘制函数，没挂 motion 控制器；补上后鼠标悬停恢复竖线、时间点读数与峰值标记 |
| 5.85 | **核显显存拆分 GTT/VRAM** | AMD 采样改为 VRAM（专用）与 GTT（共享系统内存）分开上报；界面核显显示两行：「共享内存 (GTT) x / y MB」与「专用显存 (VRAM) x / y MB」，独显仍显示「显存 (VRAM)」；对旧守护进程的字段顺序做兼容纠正 |
| 5.84 | **核心方块内嵌到页面卡片** | 取消「物理核心视图」浮窗；线程方块直接嵌入 CPU 页大卡片曲线下方（按物理核分组、每格显示线程号与使用率），自动读取核心/线程数并按可用宽度缩放排布（16C/32T 自动换行），悬停显示线程详情 |
| 5.83 | **网络/磁盘改为双曲线顶部数据** | 网络页改为「接收(蓝)/发送(绿)」双曲线，磁盘页改为「读取(橙)/写入(紫)」双曲线；实时值以大号彩色文字显示在曲线带名处（透明背景），接口数/累计/繁忙/累计次数移到标题下方摘要行；删除底部重复数值卡片与磁盘详情中的速率行 |
| 5.82 | **去重复数据 + 放大标签** | 页面底部不再重复图表带名已有的数值（CPU 使用率/温度卡、内存 Swap 使用率、AMD 利用率）；曲线带名字号 11→14、悬停读数 11→12.5，带新增高 26px 保证不压曲线 |
| 5.81 | **AMD 驱动版本号** | 自编译驱动没有 pkg 元数据，改为扫描 `/boot/modules/amdgpu.ko` 内嵌的构建路径 `drm_v6.12.110`（二进制用 memmem 搜索），显示 `drm-kmod v6.12.110`；找不到模块时回退 `amdgpu (FreeBSD DRM)` |
| 5.80 | **内存条信息（ABI 19）+ GPU 空字段隐藏** | 内存页新增「内存条」卡片：插槽总数/已安装条数、每条容量/类型/标称与实际速率/厂商（`dmidecode -t 17`，守护进程 root 采集）；GPU 详情不再显示无数据的行（序列号/风扇/BAR1/ECC/PCIe/电压电流等按可用性隐藏） |
| 5.78 | **修复拖选文本崩溃（SIGBUS）** | 详情卡每秒重建控件树，可选中的标签被销毁后 GTK 的焦点/选区块仍引用已释放对象，拖选/复制时在 `gtk_widget_is_ancestor` 处 SIGBUS。详情值标签改为不可选中（数据用「导出数据」），拖选/点击压力测试 0 崩溃 |
| 5.77 | **修复 SIGBUS 异常退出** | 两点：①页头/曲线合并成卡片后，核心方块与 GPU 下拉框仍往旧容器插入，GTK 断言失败导致控件处于游离状态，后续控件树遍历崩溃——改为按实际父容器插入；②网卡/磁盘/GPU 详情卡每秒销毁重建控件树，改为内容签名不变则跳过，显著降低 GTK 遍历压力 |
| 5.76 | **GPU 子项命名 + AMD 元数据** | 子列表改名为 GPU 1/2/3（不用设备名）；AMD 卡通过 `sys.device.drmnN.fw_version.smc_fw_version` 识别 DRM 设备，读取 VBIOS 与 unique_id，驱动显示 amdgpu (FreeBSD DRM) |
| 5.75 | **显卡页并入概览分组** | 取消独立「显卡」主导航；GPU 页面（gpu0..gpu3）作为「概览」子项，按实际检测结果展示设备名；`NavGroup.pages` 扩容到 12 以容纳更多子项 |
| 5.74 | **每张显卡独立页面** | 新增「显卡」分组：按检测到的 GPU 动态生成子页面（gpu0..gpu3），标题为设备名，副标题显示厂商/核显独显/传感器状态；每页独立曲线（各卡固定颜色）与「显卡详情」卡片；流处理器数量驱动提供时显示，否则为 — |
| 5.73 | **CPU 频率/插槽/虚拟化 + NVIDIA 明细（ABI 18）** | CPU 页新增插槽、逻辑处理器、基准频率（hw.clockrate）、当前频率（dev.cpu.0.freq）、虚拟化（vmm）；GPU 页新增显卡详情卡片：驱动/VBIOS/UUID、显存、利用率细分（GPU/显存/编码/解码/JPEG/OFA）、时钟（图形/SM/显存/视频）、功耗、风扇、PCIe 速率与吞吐、BAR1、ECC、P-State |
| 5.72 | **网卡/磁盘硬件明细（ABI 17）** | 共享内存新增：接口类型、连接状态、速率、MAC、IPv4/IPv6、SSID、信道、信号强度、网卡芯片型号；磁盘型号、序列号、容量、SSD/HDD 类型。守护进程用 getifaddrs + ifconfig + pciconf + diskinfo 采集（子进程在写锁外），界面新增「网络接口详情」「磁盘设备详情」卡片 |
| 5.71 | **页头与曲线合并为一张卡片** | 标题/副标题不再单独成卡，和曲线放在同一张 `.page-card` 大卡片内（圆角+描边+投影+内边距），下方详情卡保持独立 |
| 5.70 | **页头卡片 + 厚重阴影** | 页面标题/副标题放入 `.page-header-card` 卡片；曲线卡片阴影加强（0 3px 10px / 22%），详情卡统一轻阴影，形成三层卡片层次 |
| 5.69 | **曲线卡片化 + 网格统一** | 所有曲线绘图区外包圆角卡片（`.chart-card`：描边 + 阴影 + 内边距），与内部面板形成层次；分带边框由系列色改为统一主题网格色，背景格子颜色一致 |
| 5.68 | **浅色/深色主题** | 新增 `theme.c/h`：跟随系统（GTK 4.20 `gtk-interface-color-scheme`）或手动浅色/深色；标题栏下拉切换并写入 `$XDG_CONFIG_HOME/fmon/theme` 持久化；曲线面板、网格、坐标轴、悬停框等 Cairo 配色同步切换；`--theme auto|light|dark` 可直接指定 |
| 5.67 | **内存与 Swap 合并页** | 「内存」页改为双带合并图表（内存使用率 + Swap 使用率），详情卡分「内存」「Swap」两组；导航移除独立 Swap 页 |
| 5.66 | **ZFS 四指标独立成页** | 「存储」分组拆为：ZFS 池空间 / ZFS ARC / ZFS 命中率 / ARC Prune / ZFS 带宽 / 存储空间，四个指标各自一页、各一张曲线，不再共用合并画布 |
| 5.65 | **修复关闭浮窗后崩溃（UAF）** | 核心转储回溯：`on_ui_tick → gtk_widget_queue_draw` 访问已销毁控件。浮窗销毁时未清空 `g_core_win/g_core_win_area`，1 秒定时器继续向已释放控件发重绘；现连接 `destroy` 信号清空指针，并已用 WM_DELETE 关闭浮窗验证进程存活 |
| 5.64 | **浮窗只显示线程数** | 浮窗标题简化为「当前 N 线程」（核心数已在页面底部显示），避免重复 |
| 5.63 | **内嵌核心方块 + 线程数标题** | 页面底部恢复方块但改为「一个物理核一个方块」（8 个，颜色=该核平均负载）；悬浮窗标题改为「当前 N 线程 · M 物理核 · 每核 K 线程」 |
| 5.62 | **线程级方块与精简内嵌** | 页面底部只保留「N 核 · M 线程」文字；悬浮窗改为一个线程一个方块、按物理核分组底框，自适应排布，悬停显示线程号/物理核/使用率/同核线程 |
| 5.61 | **物理核心悬浮窗** | CPU 页新增「物理核心视图」：独立浮窗按物理核分组（kern.smp.cores/threads_per_core 动态识别），枚举列数取最大方格智能排布，悬停高亮并显示各线程使用率与均值；支持 `--core-window` 启动即打开 |
| 5.60 | **小方块背景 + 每核心方块** | 面板纹理改为横竖小方块网格；CPU 页新增每逻辑核小方块（编号 + 使用率，颜色随负载），标题显示物理核/线程数 |
| 5.59 | **几何纹理背景** | 面板在渐变之上叠加裁剪到圆角的 45° 斜线网格（白色 3.2% 透明），背景不再单调平秃 |
| 5.58 | **图表面板背景** | 新增 `fmon_gui_draw_panel()`：圆角 + 上浅下深的深色渐变 + 系列色细边框；多带页与单曲线页的绘图区都与窗口背景明显区分 |
| 5.57 | **去除重复刻度 + 卡片与曲线同口径** | 删除左侧上下限刻度（与条带名后的当前值重复）；ARC 卡片「占比」改为「占内存」，命中率/Prune 与曲线同格式，底部数据与曲线完全一致 |
| 5.56 | **刻度与数值统一表达** | 左侧刻度改为右对齐、按单位压缩（38.53 MB/s / 0 B/s / 11.0%），不再被裁切或压到绘图区；带名与悬停读数共用同一格式化函数 |
| 5.55 | **统一 1 秒节奏 + 图表重构** | 采样与刷新统一为 1 Hz（曲线每秒一个点、每秒绘制一次）；`combo_chart.c` 拆成几何/网格/边框/标签/刻度/填充/曲线/悬停等独立静态函数，严格 C11 |
| 5.54 | **ZFS 带宽真实速率** | `zpool iostat -Hp` 输出的是自导入以来的均值，差分恒为 0；改为 `-y 1 1` 取 1 秒区间值，子进程移到写锁外执行，累计值按采样累加 |
| 5.53 | **守护进程重启自动重连** | GUI 在数据持续过期时每 8 秒重新 attach 共享内存，解决“时钟在走、数据冻结”的旧映射问题 |
| 5.52 | **详情与曲线同步** | UI 刷新从 1s 提到 250ms（与采样同频）；磁盘读写详情改为最近 1 秒均值，避免瞬时采样落在两次爆发之间显示 0 |
| 5.51 | **去除硬编码前缀** | meson 统一用 `--prefix`：安装目录/rc.d/许可证/语言包均由前缀派生；C 代码用 `FMON_LOCALBASE` 宏；`install.sh --prefix`；rc.d 模板 @PREFIX@ 替换；pkexec/rc.d 搜索列表也按前缀生成 |
| 5.50 | **nvidia-smi 路径自适应** | 采集器搜索 PATH + 常见目录（含 /usr/local/nvidia/bin），并支持 FMON_NVIDIA_SMI 覆盖；安装脚本同样按 PATH→目录→覆盖顺序检测，不再依赖单一硬编码路径 |
| 5.49 | **多显卡切换与更新时间** | GPU 页加显卡下拉框（AMD/NVIDIA 可切换，复用已有探测数据）；顶部显示「更新 时:分:秒」；安装脚本检测 nvidia-smi/nvtop 并给出安装提示 |
| 5.48 | **去重型号行** | CPU/GPU 型号只保留在标题下方副标题，移除卡片中的重复「型号」行；GPU 详情改为负载/显存/温度三行 |
| 5.47 | **分带边框与型号显示** | 每条带加独立边框；CPU 页副标题与「运行」卡片显示 CPU 型号；GPU 页副标题显示自动探测到的型号与厂商 |
| 5.46 | **曲线渐变填充** | 每条曲线下方按系列颜色绘制垂直渐变（顶部 30% 不透明度 → 底部 2%），与旧卡片观感一致 |
| 5.45 | **悬停读数跟随光标** | 合并图读数框从固定右上角改为跟随鼠标（右下偏移，靠边自动翻转到左侧/上方并夹紧边界） |
| 5.44 | **横幅闪烁修复** | 采集线程持锁时快照会瞬时失败，原先被误判为“守护进程挂起”；现在瞬时失败保持上一次在线状态，只有数据真正超过 5 秒未更新才提示 |
| 5.43 | **刻度回退** | 分层 5 级刻度过于密集，回退为只标该带上下限的动态刻度（保留带名与当前值） |
| 5.42 | **分层刻度** | 每条带左侧由“仅上下限”改为 5 级等距刻度（数值随量程动态插值，量程<1 保留两位小数），并绘制对应参考线 |
| 5.41 | **分带刻度与大字号卡片** | 每带左侧显示上下限刻度（小量程两位小数、非负指标不为负）；详情区改为横向分类卡片（表头 + 16px 数值） |
| 5.40 | **合并图改分带显示** | 多曲线不再叠在一起：每条曲线一条横带（带内自动量程 + 色块标签 + 当前值），共享时间轴，悬停跨带读数 |
| 5.39 | **ZFS 带宽实时化** | 池容量 10s、I/O 1s 分开采样（原先 I/O 也跟着 10s，曲线像冻住）；线程循环 2s→500ms |
| 5.38 | **合并页与自动量程** | 多曲线图表改为每条曲线按自身 min/max 自动缩放（波动可见）、修正时间轴跨度；CPU 页合并使用率+温度；电池并入概览；移除空的硬件分组 |
| 5.37 | **网络拆分** | 「网络」分组下直接列「活动连接」「网络接口」两页，去掉页面内 Notebook；两个视图各自维护刷新定时器与可见性判断 |
| 5.36 | **ZFS/硬件大页面** | 存储分组拆为 ZFS 池空间（含多池表格）/ARC/命中率/读带宽/写带宽/ARC Prune/存储空间；硬件分组拆为 CPU 温度/电池。全部复用大页面组件（实时曲线 + 悬停读数 + 每秒详情） |
| 5.35 | **内存可用口径** | 共享内存新增 `mem_avail_bytes`（ABI 16），采集 `free + inactive + cache`；内存页「可用/已用/使用率」按此口径显示 |
| 5.34 | **按指标的性能页** | 概览拆为 CPU/内存/Swap/网络/磁盘/GPU 六个独立页：全宽大曲线 + 下方实时详情卡（每秒更新），复用原有曲线数据与绘图函数 |
| 5.33 | **二级列表按需滑出** | 启动全宽显示系统监控，二级列表用 `GtkRevealer`（slide-right，260ms）在首次点击分组时滑出；启动阶段的程序化选中通过 ready 标志忽略，不会误触发 |
| 5.32 | **固定二级列表** | 删除“分组磁贴页”，改为左分组 + 中常驻子项 + 右内容：点分组立即显示首个子页，点子项平滑切换，内容自动铺满剩余宽度 |
| 5.31 | **两级导航** | 左侧 6 个分组，右侧分组卡片页（图标 + 标题 + 说明磁贴），点磁贴进入页面；`--page` 自动定位所属分组；启动页仍为系统监控 |
| 5.30 | **Windows 11 视觉风格** | 全局样式覆盖：强调蓝选中态 + 左侧指示条、扁平卡片/按钮/输入框、表格行高 40、标签页下划线；进程页新增底部操作栏（结束任务 / 详细信息） |
| 5.28 | **进程内存列改为实际占用** | `ps` 增加 `rss` 列，界面按 KB/MB/GB 显示，排序与 CSV 同步；单位标签统一为 KB/MB/GB（仍按 1024 换算） |
| 5.29 | **导航去重** | 删除与「网络连接→网络接口」重复的「细分监控」页；磁盘明细移到「存储空间→磁盘设备」标签页 |
| 5.26 | **细分监控** | 共享内存新增 `disks[]`（ABI 15），`devstat(3)` 按设备统计读/写速率、IOPS、繁忙度；新「细分监控」页并列展示网卡与磁盘明细 |
| 5.27 | **指标历史持久化** | 守护进程每 10 秒写 `history.tsv`（20 MiB 轮转），「历史记录」页支持 1 小时 / 24 小时 CPU、内存曲线与平均/峰值 |
| 5.24 | **rctl 资源限制** | 新「资源限制」页：列出规则、添加/删除（控制通道 `RCTL`，严格白名单校验）；未启用 RACCT 时给出 loader.conf 指引 |
| 5.25 | **cpuset 管理** | 进程右键「CPU 绑定…」：读取当前 mask、修改列表；控制通道 `CPUSET`，自己的进程直接调用、其他用户回退 pkexec |
| 5.23 | **告警体系** | 分级（警告/严重）、告警历史落盘（`~/.local/state/fmon/alerts.log`，2 MiB 轮转）、独立「告警历史」页（最近 300 条、静默 1 小时、清空）、通知按级别区分标题 |
| 5.21 | **曲线精度与观感** | 采样 1 Hz → 4 Hz（250 ms），历史 120/2 分钟 → 1200/5 分钟；单调三次（Fritsch–Carlson）插值，杜绝过冲；悬停竖线 + 数值/时刻读数；时间戳精确到微秒 |
| 5.22 | **界面细节** | 按需求保持全局粗体、固定两列（左 3 行/右 3 行）、默认窗口放大到 1500×920；**不记忆**窗口状态/页面/显卡，每次启动固定首页；移除与侧边栏重复的页面大标题 |
| 5.20 | **gettext 国际化** | `po/` + `i18n.gettext` + `_()` 包裹界面文案（123 条 msgid），`bindtextdomain` 初始化；无翻译时回退中文 |
| 5.17 | **界面美化** | 卡片圆角/阴影/左侧强调色、侧边栏与导航重绘、表格 hover 与选中效果、滚动条细化、按钮/输入框统一圆角；字体未改 |

---

## 6. 性能与资源（改造前后）

| 项目 | 旧版 | 新版 |
|---|---|---|
| 磁盘统计 | 每 100 ms `popen(iostat \| awk)`（shell+iostat+awk，`iostat` sleep 1 秒） | `devstat(3)` 系统调用，0 子进程 |
| GPU 探测 | 每 100 ms `popen(nvidia-smi)`（无卡时还会伪造数据） | 每 5 秒一次；NVIDIA 走 NVML、AMD 走 ioctl；无卡 60 秒重试一次并显示 N/A |
| ZFS | 每次 `zpool list \| head`、`zpool iostat … \| tail`（阻塞 1 秒）、`swapinfo \| awk` | `zpool list/iostat` 每 10 秒各一次（直接 exec），`swapinfo` 每 5 秒一次；ARC 走 sysctl |
| 采样节拍 | 100 ms～2 s 混杂，索引推进 2 次/轮 | 统一 1 Hz；发布线程唯一推进历史 |
| GUI 子进程 | 日志/进程/服务/网络页面在主线程同步 popen，界面卡顿 | 全部 `GTask` 后台线程，主线程不阻塞 |
| 隐藏页面 | 所有页面定时器持续轮询 | 只在页面可见（mapped）时工作 |
| 刷新频率 | 500 ms 重绘 14 张曲线 | 1 s（数据本身就是 1 Hz） |
| 实测 CPU | — | daemon ≈0.0–0.1%；GUI（守护进程在线、鼠标不在窗口上）：sys 0.4%、proc 1.7%、zfs 1.7%、net 0.7%、storage 0.5%、hw 0.3%、svc 0.1% |
| 内存 | — | GUI RSS 稳定在 ~315 MB（GTK4 + Mesa/llvmpipe 软件渲染占绝大部分，2 秒时即已达 316 MB，与业务代码无关），90 秒采样无增长 |

> 说明：新版已按视图取数据并用数据指纹跳过无变化重建，`proc` 页从 4.2% 降到约 1.7%；
> 以 root 运行“系统视图”时仍会解析全部进程，若只想看自己的进程，用普通用户运行即可。

---

## 7. 代码规范与工程改进

* 全量编译 **0 warning**（clang 19，含 `-Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings -Wvla`）。
* 统一命名：`fmon_*`（公共）、`gui_*`（界面）。
* 所有公共函数都有头文件声明；不再有“隐式声明”或函数指针滥用。
* `G_DEFINE_TYPE` 生成的 `*_get_type()` 显式前置声明，满足 `-Wmissing-prototypes`。
* 结构体字段全部定长类型，并加 `_Static_assert` 约束大小与 `magic` 偏移。
* 共享内存读写全部集中到 `fmon_shm.c`，两端复用同一份校验逻辑。
* 删除空文件与重复实现；`Makefile` 与 meson 的文件列表保持一致。
* 新增 rc.d 脚本、一键安装脚本、README、本报告、`docs/` 截图。

---

## 8. 后续建议（未在本次实施）

1. **控制通道**：把“结束进程/服务操作”从 GUI 的 `pkexec` 改由守护进程通过 Unix 域套接字执行（`getpeereid()` 校验对端 uid/gid），这样可以在守护进程侧做 PID+启动时间复核，避免 PID 复用误杀，也便于审计。
2. **能力模式**：守护进程初始化完成后 `cap_enter()`（Capsicum），只保留已打开 fd 的访问能力。
3. **进程列表按 pid 增量绑定**：当前是“数据变则整表重建”，进一步可用 GObject 属性 +
   `g_object_bind_property` 做单元格级更新，把 root 全量视图的 CPU 再降一个量级。
4. **Intel 显卡负载**：FreeBSD 的 i915 目前没有对应传感器；可关注 `dev.drm.*` / hwmon
   的新接口，或通过 `libdrm_intel` 的 perf ioctl 扩展。
6. **告警阈值**：CPU/内存/磁盘/温度超过阈值时通过 `notify-send` 或桌面通知提醒。
7. **单元测试**：为共享内存 seqlock、`fmon_child`、rc.conf 解析、`ps`/`sockstat` 解析增加 `meson test` 用例（这些函数已与 GTK 解耦，可直接测试）。
8. **CI**：GitHub Actions / Cirrus CI 上跑 `meson setup && ninja`，并加 `-Werror`。
9. **打包**：提供 `pkg-plist` / ports 骨架与 man page（`fmon-daemon.8`、`fmon-gui.1`）。
10. **国际化**：界面文案目前是中文硬编码，可改为 gettext。
11. **渲染器**：在无 GPU 的虚拟机上可用 `GSK_RENDERER=cairo` 降低显存占用；实测该组合在 GTK 4.20 下退出阶段会触发 GTK 内部 `g_datalist_clear` 崩溃（栈帧全部在 libgtk/libgobject 内，与业务代码无关），因此**没有**在代码里强制该渲染器，仅在此记录。

---

## 9. 变更文件清单

```
新增
  install.sh                      一键安装 / 卸载脚本
  src/common/fmon_child.[ch]      无 shell 的子进程执行（超时/限流）
  src/common/fmon_shm.c           共享内存创建/校验/seqlock/快照
  src/gui/gui_util.[ch]           异步命令、对话框、格式化、合法性校验
  src/gui/gui_state.h             界面统一快照访问接口
  src/gui/storage_view.[ch]       存储空间页面（新功能）
  src/gui/alerts.[ch]             阈值告警 + 桌面通知
  src/daemon/collector_net.c      网卡/接口采集（从 collector_io 拆出）
  src/daemon/collector_gpu.c      GPU 自动探测 + NVIDIA/AMD 负载采集
  src/daemon/collector_storage.c  挂载点 + Swap 采集
  src/daemon/fmon_sysctl.h        sysctl/时间 小工具（头文件内联）
  rc.d/fmon_daemon                FreeBSD 启动脚本
  docs/*.png                      界面截图
  README.md                       项目说明（原为空文件）
  CODE_REVIEW.md                  本报告

重写
  src/common/fmon_shm.h           带版本/大小/magic 的新 ABI（v13，含多 GPU（驱动/槽位）/ 多 zpool 字段）
  src/daemon/fmon_daemon.c        单实例、信号、心跳、发布线程、--version
  src/daemon/fmon_daemon.h
  src/daemon/collector_cpu.c
  src/daemon/collector_io.c       devstat（GPU 拆到 collector_gpu.c）
  src/daemon/collector_zfs.c      sysctl ARC + 单次 zpool iostat
  src/daemon/collector_proc.c     kvm 按 pid 聚合 + Top N
  src/gui/main.c                  快照、横幅、卡片、Top 进程、GPU 切换、CSV、--page/--gpu
  src/gui/gui_chart.[ch]          忠实绘图、单位、有效点、空状态提示
  src/gui/process_view.[ch]       排序/自动刷新/信号/导出
  src/gui/service_view.[ch]       rc.conf 解析/过滤/确认
  src/gui/network_view.[ch]       连接 + 接口双标签页
  src/gui/log_view.[ch]           异步/过滤/暂停/文件选择
  meson.build  src/daemon/meson.build  src/gui/meson.build  Makefile

删除
  src/common/fmon_utils.h         （0 字节空文件）
  src/gui/gui_window.c            （0 字节空文件）
  fmon-gui.core                   （旧版本崩溃残留，270 MiB，无匹配符号）
```

---

## 10. 如何验证本次改造

```sh
# 1. 一键安装（推荐）：装依赖 + 编译 + 安装 + 自启 + 启动 + 自检
sudo ./install.sh

#    预演（不改系统）/ 只编译 / 卸载
./install.sh --dry-run -y
./install.sh --build-only -y
sudo ./install.sh --uninstall

# 2. 手动构建（应 0 warning）
rm -rf build && meson setup build && ninja -C build

# 3. 手动运行（需要 root：kvm / 部分 sysctl / /var/run / /dev/dri）
sudo ./build/src/daemon/fmon-daemon
./build/src/gui/fmon-gui                 # 另开终端（普通用户）
./build/src/gui/fmon-gui --page proc     # 直接打开指定页面
./build/src/gui/fmon-gui --gpu 1         # 多卡机器上默认显示第 2 张显卡

# 4. 检查 GPU 探测（两张卡都应出现）
sysctl hw.nvidia.gpus.0.model             # NVIDIA 型号
sysctl dev.drm.0.PCI_ID dev.drm.1.PCI_ID  # DRM 设备 PCI ID
./build/src/daemon/fmon-daemon --version  # 版本 + ABI + AMD 传感器是否启用

# 5. 检查数据一致性
sysctl kern.cp_time                  # 与界面 CPU 百分比对照
swapinfo -k                          # 与 Swap 卡片对照
zpool list -Hp -o name,size,alloc    # 与 ZFS 页面对照

# 6. 安装为服务
sudo ninja -C build install
sudo sysrc fmon_daemon_enable=YES
sudo service fmon_daemon start
```

**升级注意**：

* 新版本改用共享内存 `v11`，与旧版 `v9/v10` 互不影响，但**不要同时运行两个守护进程**。
* `/etc/rc.conf` 里原有的 `fmon_daemon_enable="YES"` 仍然有效；`install.sh` 会先停掉旧进程、
  把 `/usr/local/bin/fmon_daemon`（下划线命名）备份为 `.bak`，再安装并启动新版本。
* 旧 GUI 会被新 daemon 拒绝并提示“ABI 不兼容”（不会崩溃），记得一起替换。
