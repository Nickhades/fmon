# 全功能探测契约与审计

本文件把“全硬件通用、智能探测、绝不伪造”落实到每个功能，而不只是磁盘。
规则按优先级执行：

1. **内核接口优先**：有稳定的 FreeBSD 内核 API/ioctl/sysctl，直接读真实值。
2. **无接口显示不可用**：用 `NaN` / `-1` / `*_valid=0`，界面显示 `—`。
3. **不硬编码**：设备名来自 `kern.disks`/`devstat`/`getifaddrs`/`getmntinfo`；
   控制器由 `NVME_GET_NSID` 反查；工具路径从 `PATH` 解析；不写死 `/dev/nvme0`。
4. **工具回退**：没有内核接口时，允许 FreeBSD base 工具，或检测到的可选依赖；
   必须记录来源；全部失败时显示不可用。**不得用 0 冒充未知值。**
   `install.sh` 会自动检测并安装 `dmidecode`、`smartmontools`、`libdrm`；
   系统已有则跳过，`--skip-deps` 可显式禁用自动安装。

## 1. 当前审计矩阵

| 功能 | 当前来源 | 优先级 | 状态 |
|---|---|---|---|
| CPU 使用率 / 每核 | `kern.cp_time` / `kern.cp_times` sysctl | 内核 | 符合 |
| CPU 温度 | `dev.cpu.N.temperature` / ACPI thermal sysctl | 内核 | 符合（无传感器 NaN） |
| 内存 / 可用内存 | `vm.stats` sysctl + pagesize | 内核 | 符合 |
| 负载均值 | `vm.loadavg` sysctl | 内核 | 符合 |
| Swap | 首选 `kvm_getswapinfo`；失败回退 `swapinfo`；`FMON_PROBE_SWAP` | 内核 | 符合 |
| 电池 / 电源 | ACPI `hw.acpi.*` sysctl；`FMON_PROBE_BATTERY` | 内核 | 符合；不再推断 `acline` |
| 磁盘 I/O | `devstat(3)` | 内核 | 符合 |
| 磁盘设备名 | `kern.disks` + devstat | 内核 | 符合 |
| 磁盘 SMART | NVMe ioctl / CAM ATA / CAM SCSI；回退 nvmecontrol / smartctl | 内核 | 符合（带 `smart_source`） |
| 磁盘型号/序列号/转速/接口 | `DIOCG*` + NVMe controller + CAM ATA/SCSI；失败回退 `geom`/`diskinfo` | 内核 | 内核优先；`meta_valid` 标记 |
| 网络计数器/地址 | `getifaddrs` + `if_data` sysctl | 内核 | 符合 |
| Wi-Fi SSID/信道/信号/速率 | `ifconfig` 文本；`detail_valid` 标记 | 工具 | **待迁移**：`IEEE80211_IOC_*` |
| 网卡型号 | `pciconf -lv` | 工具 | **待迁移**：`PCIOCGETCONF` |
| GPU 存在/型号 | sysctl + `pciconf` | 混合 | 部分；AMD/NVIDIA 驱动路径优先 |
| GPU 负载/温度 | AMD `libdrm_amdgpu`；NVIDIA `nvidia-smi`（可选） | 厂商 | 允许；无接口显示不可用 |
| 挂载点容量 | `getmntinfo` + `statvfs` | 内核 | 符合 |
| 进程快照 | `kvm_getprocs` | 内核 | 符合 |
| 内存条 SPD | `dmidecode`（可选包）；`FMON_PROBE_MEM_MODULES` | 工具 | 允许；无通用内核/用户态 ABI |
| ZFS ARC | `kstat.zfs` sysctl | 内核 | 符合 |
| zpool 容量/状态/I/O | `zpool`（base）；`FMON_PROBE_POOLS` | 工具 | 允许；无稳定公开 ioctl |
| 服务/进程管理动作 | `service` / `sysrc` / 信号 | 工具 | 管理动作，按设计使用 |
| GUI 电池详情 | `acpiconf`（base） | 工具 | **待迁移**：核心数据已在 daemon sysctl |
| GUI 磁盘附加信息 | `geom`（base） | 工具 | 可改用 daemon 快照 |
| GUI 磁盘队列 | `iostat`（base） | 工具 | 可用 devstat 扩展，待迁移 |
| GUI kmem/net 详情 | `vmstat -z` / `netstat` / `ifconfig` | 工具 | 待迁移/可保留为详情 |
| GUI zpool 健康 | `zpool`（base） | 工具 | 允许；无统一 API |

## 2. 数据完整性风险（需要继续修）

ABI 23 用 `probe_valid` 位图区分“真实 0”和“探测失败”：

- `FMON_PROBE_SWAP`：Swap 成功才置位；失败显示 `—`。
- `FMON_PROBE_POOLS`：zpool 命令成功才置位；无池与执行失败可区分。
- `FMON_PROBE_MEM_MODULES`：dmidecode 成功才置位；无内存条与失败可区分。
- `FMON_PROBE_MOUNTS` / `FMON_PROBE_PROCS` / `FMON_PROBE_ZFS_ARC` / `FMON_PROBE_BATTERY`。
- `FmonDiskInfo.meta_valid`：磁盘元数据是否至少部分可用。
- `FmonIfaceInfo.detail_valid`：Wi-Fi/速率详情是否解析成功。
- 电池不再根据放电标志推断 `acline`；缺失就保持未知。

仍存在的风险：部分工具失败时可能保留上一次值（例如 GPU 负载），下一步补 stale 标记。

## 3. 下一步迁移顺序

1. Swap：已完成 `kvm_getswapinfo` + `FMON_PROBE_SWAP`。
2. 磁盘元数据：已完成 `DIOCGMEDIASIZE` / `DIOCGIDENT` + NVMe controller + CAM ATA/SCSI。
3. Wi-Fi：`IEEE80211_IOC_SSID` / `CHAN` / `RSSI` ioctl，去掉 `ifconfig` 解析。
4. 网卡型号：`PCIOCGETCONF` ioctl，去掉 `pciconf`。
5. 电池：把 `acpiconf` 详情移入 daemon sysctl/快照。
6. GUI 详情：优先读 daemon 快照；确实没有快照的再走 base 工具。
7. 为每个 provider 增加“内核成功 / 工具回退 / 不可用”三态测试。
8. 处理工具失败时可能保留旧值的 stale 标记（GPU 等）。

## 4. 不允许的实现

- 不写死 `/dev/nvme0`、`/dev/ada0`、`nvmeN` 到 `ndaN` 的映射。
- 不通过 shell 拼接设备名；统一 `fmon_child_run(argv)`。
- 不在探测失败时填 0、空字符串或旧值冒充当前值。
- 不把厂商工具/可选包设为硬依赖；检测不到就显示不可用。
- 不把“没有传感器”和“真实 0% / 0°C”混为一谈。
