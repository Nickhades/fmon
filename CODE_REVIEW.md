# fmon 1.3.3 代码审核报告（1.3.2 复审修复）

> **审核对象**：fmon 1.3.2
> **复审修复**：fmon 1.3.3
> **代码基线**：`main` 提交 `b8e7558`（`v1.3.2` 标签提交为 `15c2955`，代码相同）
> **审核日期**：2026-11-22
> **审核环境**：FreeBSD 15.1-RELEASE-p7 amd64，clang 19.1.7，GTK 4 / Cairo，Meson + Ninja
> **共享内存 ABI**：24
> **审核范围**：`src/` 下 C/H 代码、Meson 构建、单元测试、运行时边界与数据真实性
> **结论**：整体架构和构建质量良好。初次静态分析发现 1 项高优先级内存安全缺陷（日志上下文 use-after-free）；已在 1.3.3 中修复，复审静态分析为 **0 条告警**。

---

## 1. 基线与验证结果

| 项目 | 结果 |
|---|---|
| 版本 | `fmon-gui 1.3.3 (共享内存 ABI 24)` |
| 源文件规模 | 46,565 行 C/H（不含生成物） |
| 最大业务文件 | `gui_window.c` 684 行；最大文件 `i18n_phrases.c` 739 行为生成数据 |
| 编译告警 | 0 warning |
| 单元测试 | 20 / 20 通过 |
| 共享内存结构 | `sizeof(FmonShmData) == 226,832` 字节，小于 1 MiB |
| ABI/大小校验 | magic + ABI + `struct_size` 三重校验 |
| 初次静态分析 | scan-build：5 条诊断，其中 1 条真实 UAF |
| 复审静态分析 | scan-build：**0 条诊断** |

复现：

```sh
cd fmon4
ninja -C build
meson test -C build
scan-build --status-bugs -o /tmp/fmon-scan ninja -C build-scan
./build/src/gui/fmon-gui --version
```

---

## 2. 架构审核

### 2.1 进程边界

- `fmon-daemon` 以 root 运行，负责特权采集与控制操作。
- `fmon-gui` 以普通用户运行，只读映射共享内存。
- GUI 不直接打开 `/dev/nvme*`、不直接执行特权 sysctl；数据通过 daemon 发布。
- 控制通道为本地 Unix 域套接字，文件权限 `0660`，属组优先 `operator`，不存在时退回 `wheel`。

这是正确的 FreeBSD 桌面监控拆分方式：采集器集中、GUI 无特权、控制面有明确审计边界。

### 2.2 共享内存一致性

- 头部固定字段：`magic`、`abi_version`、`struct_size`、`seq`、`daemon_pid`、`status`。
- 写入使用 seqlock：写事务把 `seq` 置奇数，写完置偶数；GUI 用 `fmon_shm_snapshot()` 前后校验 `seq`，失败最多重试 64 次。
- 读取前先检查文件大小和头部，不再直接 `mmap` 后越界读取。
- 结构体有静态断言：`sizeof < 1 MiB`、`magic` 偏移为 0、`seq` 4 字节对齐。
- 写者只有一个，历史数组只推进一次，避免旧版“曲线只更新一半”的问题。

### 2.3 模块组织

代码按职责拆分，未出现单文件数千行的业务模块：

- `src/common/`：共享内存、子进程、SMART。
- `src/daemon/collectors/`：CPU/内存/IO/网络/ZFS/GPU/进程/存储。
- `src/daemon/core/`：daemon 主循环、控制通道、历史落盘。
- `src/gui/app/`：窗口、导航、主题、语言切换、导出。
- `src/gui/pages/`：页面布局与业务展示。
- `src/gui/views/`：进程、服务、网络、日志、历史等视图。
- `src/gui/charts/`：Cairo 折线/阶梯图、几何与交互。
- `src/gui/support/`：格式化、告警、磁盘历史、日志渲染等。

旧版根目录的过期审核/交接文档已从 main 清理，仅通过历史标签访问。

---

## 3. 数据真实性

这是本项目相对普通“系统监控玩具”的核心差异。

- 探测失败与真实 0 分开：`probe_valid` 位图、`meta_valid`、`smart_valid`、`detail_valid`。
- 不可用数据用 `—` 或 NaN，不伪造 0、5%、50%、99% 之类曲线。
- 图表使用真实时间戳、线性/阶梯路径和 NaN 断点；`test_no_bezier` 防止回退到 Bézier/样条。
- 磁盘温度/寿命优先 NVMe ioctl、CAM ATA/SCSI，失败才回退可选工具；工具失败显示不可用，不猜。
- 网卡型号字段为 192 字节，网络页整行换行显示，不省略。
- 磁盘历史只列出当前快照中的在线磁盘；外置盘拔出后从选择器消失，重新插入后继续记录。
- 页面隐藏时跳过不必要的明细刷新，避免为了“看起来在动”而制造数据。

---

## 4. 并发与内存安全

### 4.1 已确认良好的部分

- `fmon_shm_snapshot()` 对整结构做 `volatile` 逐字节拷贝，前后校验 seqlock。
- 子进程执行改用 `posix_spawnp()`，参数以 argv 传递；采集器不再使用 `popen("/bin/sh -c ...")`。
- `fmon_child_run()` 有输出上限、超时和 SIGKILL 兜底，避免子进程无限输出或挂死。
- 控制通道的 pid 解析拒绝 `pid <= 1` 和自身，信号只允许 TERM/KILL/STOP/CONT/HUP。
- CPU 列表只允许数字、逗号、连字符；服务名/用户名走 token 校验。

### 4.2 已修复：F-01 日志上下文 use-after-free

位置：`src/gui/views/log_context.c`。

原始问题：

```c
g_free(line);

/* 没点中 PID：弹出该行前后 5 行上下文 */
...
gchar *ctx = log_render_raw_context(page, line);
```

`line` 在调用 `log_render_raw_context()` 之前已经释放，后续还有 `g_strdup(line)`。
触发条件：点击/双击日志行，且该行没有 PID 标签，进入“弹出上下文”路径。
影响：读取已释放内存，可能崩溃、显示乱码，或在校验器下报 heap-use-after-free。

修复：

- 删除提前的 `g_free(line)`；
- `line` 保留到缓存命中、异步抓取、普通显示分支使用完毕；
- 每个 return 前释放 `line`；
- 复审 scan-build 不再报告该路径。

修复后逻辑经构建、20 项测试和静态分析复核；本报告不再把该问题列为未解决。

---

## 5. 静态分析结果

使用：

```sh
scan-build --status-bugs meson setup build-scan --buildtype=debug
scan-build --status-bugs ninja -C build-scan
```

初次审核：5 条诊断。修复后复审：**0 条**。

| ID | 位置 | 告警 | 处理 |
|---|---|---|---|
| F-01 | `log_context.c` | Use of memory after it is freed | **已修复** |
| F-02 | `service_list.c` | `page->names` 可能为空 | 已加防御性 guard |
| F-03 | `fmon_daemon.c` | `strlcpy` 参数可能为空 | 已加 `shm == NULL` guard |
| F-04a | `collector_gpu.c` | EOF 状态继续 fread | 改用 `read(2)`，已消除 |
| F-04b | `collector_gpu.c` | 失败操作后流位置不确定 | 改用 `read(2)`，已消除 |

### F-02 说明

`svc_scan_scripts()` 使用 `g_ptr_array_new_with_free_func()`，GLib 分配失败会终止进程，不返回 NULL；`ServicePage` 构造时立即赋值。当前不会因此崩溃，但已加：

```c
if (page == NULL || page->names == NULL)
    return;
```

用于消除分析噪声并防止未来生命周期变化引入空指针。

### F-03 说明

`fill_static_info(g_shm)` 只在 `fmon_shm_create()` 成功、`g_shm != NULL` 后调用；静态分析器未完全跟踪该路径。已加 `shm == NULL` 防御，既消警也避免未来调用方式变化。

### F-04 说明

`detect_amd_driver_version()` 改为用 `open()` + `read()` 按块搜索 `.ko` 中的 `drm_v` 字符串，不再使用 `fopen/fread`，因此不存在 stdio 在 EOF/错误后的流状态歧义。

---

## 6. 安全边界

### 6.1 已确认的防护

- 采集/控制命令不再拼接 shell；`posix_spawnp` + argv。
- 控制通道使用 `getpeereid()`，并按补充组检查 `operator`/`wheel` 成员。
- 套接字请求有最大长度和收发超时。
- 服务名、CPU 列表、pid、信号均有白名单/范围校验。
- 共享内存 0644 只读映射；写端只在 root daemon；旧版 0666 可写问题已消除。
- GUI 不直接访问特权设备；控制操作由 daemon 执行。

### 6.2 仍需明确的设计风险

1. **控制通道权限较宽**：加入 `operator` 或 `wheel` 的本地用户可以请求结束任意 pid、启停服务、改 nice/cpuset。这是“可信本地管理员组”的边界，不是普通用户边界；生产环境不要随意把用户加入该组。
2. **共享内存可读**：0644 使本机任意用户可读取进程/网络等监控数据；这是 GUI 无特权读取的取舍。若机器是多用户高敏感环境，应改为按组授权或更严格的 ACL。
3. **日志页仍有两个常量 shell 入口**：日志源的 `dmesg`/固定命令通过 `/bin/sh -c` 执行，日志上下文用固定脚本并把行内容作为位置参数传入。当前输入不拼进脚本，风险可控；后续维护不要把这些入口改成字符串拼接。
4. **CSV 导出**：进程导出对 command 做了 CSV 引号转义，但未对所有文本字段统一做公式前缀处理；本地进程名可被构造为以 `=`、`+`、`-`、`@` 开头的内容。属于低风险加固项。

---

### 6.3 Daemon / 控制通道 / 历史落盘复审（2026-11-22）

本轮继续审阅 daemon 采集器、控制通道和历史持久化，未发现新的内存越界或命令注入；发现以下阻塞式 I/O 和权限边界问题。

| ID | 优先级 | 位置 | 发现 | 建议 |
|---|---|---|---|---|
| D-01 | 中 | `fmon_daemon.c` 发布线程 | `fmon_history_store_sample()` 在 `fmon_shm_wr_begin()`/`wr_end()` 之间执行，内部会写 raw/1m/5m/1h/GPU/ZFS 多个 TSV。慢磁盘会拖住写事务，其他采集线程等锁，GUI 快照出现空洞。 | 在锁内只复制所需快照，文件 I/O 移到锁外；或交给独立历史线程。 |
| D-02 | 中 | `collector_zfs.c` | `sample_pool_capacity()` 在写锁内执行 `zpool list`，超时上限 4 秒；无响应的池会阻塞发布。 | 先在锁外填充临时 `FmonPoolInfo`，锁内只合并容量字段和 I/O。 |
| D-03 | 中 | `collector_io.c` / `collector_io_meta.c` / `disk_smart.c` | `thread_io` 持写锁调用 `sample_disk()`，其中 `disk_io_fill_meta()` 回退到 `geom`/`diskinfo`，`fmon_disk_smart_probe()` 可能调用 `smartctl`/`nvmecontrol`，单次超时可达数秒。 | 元数据/SMART 探测移到锁外，使用每盘缓存，锁内只写结果。 |
| D-04 | 低 | `collector_storage.c` | `thread_storage` 持锁调用 `sample_swap()`；KVM 失败时回退 `swapinfo`，超时 2 秒。 | KVM/回退探测在锁外完成，锁内只写 swap 字段。 |
| C-01 | 低（安全加固） | `fmon_ctl.c` | `getpeereid()` 失败时没有拒绝连接，代码继续处理请求，等于认证失败时 fail-open。 | 失败即 reply ERR 并关闭连接；同时记录审计日志。 |
| C-02 | 信息 | `fmon_ctl.c` | `fmon_ctl_start()` 中 `snprintf(g_sock_path, ...)` 重复执行一次。 | 删除重复行。 |
| H-01 | 低 | `disk_history_store.c` | 磁盘拔出后的最后一个未完成分钟仍会以旧累计值写出一行；设备选择器已隐藏它，但数据库会保留一个过渡桶。 | 在 flush 时只写本分钟 `n > 0` 的设备，或在采样时标记移除设备。 |

说明：这些是**延迟/可用性和数据边界**问题，不是内存安全回归。D-01～D-03 都可能让 GUI 在慢盘、SMART 超时或 ZFS 卡住时看到旧快照；应优先在后续版本处理。

#### 控制通道补充结论

- 服务名 `ctl_valid_token()`、动作白名单、pid 范围、信号白名单、CPU 列表和 rctl 字符集校验均存在。
- `service`/`sysrc`/`cpuset`/`rctl` 均使用绝对路径 argv，不经过 shell。
- 套接字 0660，属组 operator/wheel；这是可信管理员组边界，不是普通用户边界。
- 仍需修复 C-01，避免 `getpeereid()` 异常时无认证继续处理。

#### 历史落盘补充结论

- 主 raw/1m/5m/1h 与 GPU/ZFS sidecar 都使用定长 `FILE*` 追加，未发现路径拼接注入或 SQL 拼接。
- 磁盘历史使用 SQLite prepared statement，数据库目录 0700。
- 没有发现历史数据格式解析越界；主要风险是 D-01 的锁内 I/O 和 H-01 的过渡桶。

## 7. 性能与资源

- 采集周期 1 Hz；CPU/IO/网络/ZFS/进程/存储分线程或分阶段采集。
- GUI 主线程不执行同步 `popen`；日志、服务、进程操作走异步任务。
- 页面隐藏时有更新守卫，避免不可见页面做明细刷新。
- 历史曲线使用定长数组和保峰值抽取，不会无限增长。
- 单曲线、组合图和 CPU 平均曲线的降采样缓存都以 `time_us` 指针作为 key 的一部分；时间轴数组切换时不会误用旧几何。
- 共享内存约 222 KiB，适合每帧快照拷贝。
- 旧版每 100 ms 起子进程、GUI 主线程阻塞的问题已消除。

本次审核未做长时间泄漏/压力测试；“90 秒内存平稳”只能作为历史回归结论，不能代替长时间 soak。

---

## 8. 测试覆盖

当前 20 项测试：

```text
fmon_chart              fmon_fidelity          fmon_history_provider
fmon_child              fmon_shm               fmon_process_ps
fmon_cpu_health         fmon_service_rc        fmon_mem_summary
fmon_vm_stats           fmon_zfs_stats         fmon_cpu_stats
fmon_kmem_stats         fmon_net_stats         fmon_disk_perf
fmon_disk_queue         fmon_zpool_health      fmon_no_bezier
fmon_translations       fmon_log_filter
```

覆盖较好的部分：

- 共享内存 ABI/大小/快照；
- 子进程超时/输出上限；
- 数据真实性边界（NaN/无效值）；
- 折线路径与禁止 Bézier；
- 中英文翻译完整性；
- 日志过滤、ZFS/磁盘/网络解析。

缺口：

- GUI 对话框交互（包括 F-01 的日志点击路径）没有自动化测试；
- 控制通道的真实套接字权限/并发连接没有端到端测试；
- 长时间 soak、内存泄漏和 ASan GUI 运行不是常规 CI 项。

---

## 9. 结论与建议

### 已处理

1. **F-01**：修复日志上下文 use-after-free，并完成静态分析复审。
2. F-02/F-03：加防御性 guard，消除静态分析噪声。
3. F-04：GPU 驱动版本探测改用 `read(2)`，消除 stdio 流状态告警。

### 建议后续处理

1. 处理 D-01～D-04：历史落盘、ZFS 容量、磁盘元数据/SMART、swap 回退移出写锁。
2. 处理 C-01：`getpeereid()` 失败时 fail-closed；删除 C-02 重复行。
3. 为日志行点击上下文补一个 GUI 回归测试或 ASan 冒烟步骤。
4. 统一 CSV 文本字段转义与公式前缀防护。
5. 在文档中明确 `operator`/`wheel` 控制通道权限边界，并增加控制通道并发/长时间 soak 测试。

### 总体评价

- 架构分层正确，daemon/GUI 权限边界清晰。
- 数据真实性设计是当前代码最扎实的部分。
- 构建和测试基线良好：0 warning、20/20。
- F-01 已修复，复审静态分析为 0 条告警。daemon/control/history 复审发现的主要问题是锁内阻塞 I/O（D-01～D-04）和控制通道 fail-open（C-01），属于后续性能/安全加固重点。

---

## 10. 审核方法

1. 检查当前 `main` 的 `meson.build`、`fmon_shm.h`、版本/ABI 字符串。
2. 全量 release 构建 + 20 项单元测试。
3. `scan-build` 静态分析，并在修复后复审。
4. 手工审阅共享内存、控制通道、子进程、日志、导出、磁盘历史等高风险路径。
5. 检索 `system/popen//bin/sh`、`shm_open/0666`、`strcpy/sprintf`、pid/服务名校验等模式。
6. 核对文档中的 ABI/版本号与当前代码一致。
7. 复审 daemon 采集器、控制通道和历史落盘，重点检查锁内子进程/文件 I/O。

## 11. 审核限制

- 未做形式化验证；静态分析可能误报，也可能漏报。
- 未在所有 FreeBSD 版本、所有桌面环境/Wayland compositor 上运行。
- 未做多用户恶意本地用户渗透测试。
- 本报告基于 1.3.2 审核基线与 1.3.3 复审修复；后续提交需重新审核。
