# 硬件探测覆盖与边界

> 原则：
> 1. 有标准内核接口的，直接读真实值；
> 2. 没有接口的，显示“—”；
> 3. 不硬编码设备名/控制器编号/工具路径，运行时探测；
> 4. 没有内核接口时，允许使用 FreeBSD base 工具或可选依赖作为回退，
>    但必须记录数据来源；所有回退失败时显示“—”，**数据绝不伪造**。

## 1. 直接使用内核接口的指标

| 指标 | FreeBSD 接口 | 说明 |
|---|---|---|
| CPU 使用率 / 每核 | `kern.cp_time`、`kern.cp_times` sysctl | 全硬件通用 |
| 内存 / Swap | `vm.stats`；Swap 首选 `kvm_getswapinfo`，失败回退 `swapinfo` | 全硬件通用；`probe_valid` 区分不可用 |
| 网络接口与速率 | `getifaddrs`、`if_data` | 全硬件通用 |
| 磁盘 I/O | `devstat(3)` | 全硬件通用 |
| 磁盘温度（NVMe） | `NVME_GET_NSID` + `NVME_PASSTHROUGH_CMD` | nvme(4) ioctl；失败才回退 nvmecontrol |
| 磁盘温度（ATA/SATA） | libcam `CAM ATAIO` + SMART READ DATA | 标准 ATA SMART 页；失败可回退 smartctl |
| 磁盘温度（SCSI/SAS） | libcam `CAM SCSI LOG SENSE` page 0x0D | 标准 SCSI 温度日志页；失败可回退 smartctl |
| CPU 温度 | `dev.cpu.N.temperature` / ACPI thermal | 有传感器才显示 |
| 电池 | ACPI sysctl | 台式机显示不可用 |
| ZFS ARC/命中率 | `kstat.zfs` sysctl | 需要 ZFS |

## 2. 仍依赖基础系统工具/厂商库的部分

| 指标 | 当前实现 | 为什么还不能完全内核化 |
|---|---|---|
| ZFS 池容量/状态/带宽 | `zpool` 命令 | 没有稳定、公开的 zpool ioctl；libzfs 仍是命令层 |
| GPU（NVIDIA） | `nvidia-smi` / NVML | NVIDIA 没有 FreeBSD 内核统一传感器 ABI |
| GPU（AMD） | libdrm_amdgpu ioctl | 已是内核/驱动接口，但仅 AMD |
| GPU（Intel/其他） | 仅设备信息 | 无通用负载/温度接口，显示 N/A |
| 内存条 SPD | `dmidecode` | SMBIOS 没有稳定的通用内核用户态 ABI |
| 磁盘型号/序列号/转速/接口 | `DIOCG*` / NVMe controller / CAM；失败回退 `geom`/`diskinfo` | 内核优先；`meta_valid` 标记 |
| 服务/进程详情 | `ps`、`service` | 管理动作需要调用系统管理接口 |

这些路径都通过 `fmon_child` 的 argv + 超时执行，不经过 shell；
失败时返回不可用，不会让 GUI 崩溃。

## 3. 磁盘温度 provider 链（当前）

```
ndaN / nvmeN ──> NVME_GET_NSID ──> /dev/nvmeX
                                  └─ NVME_PASSTHROUGH_CMD GET_LOG_PAGE 0x02
adaN ─────────> libcam CAM ATAIO + SMART READ DATA (0xB0/0xD0/0xC24F)
daN  ─────────> libcam CAM SCSI LOG SENSE page 0x0D
失败 ─────────> smart_valid = 0，GUI 显示 “—”
```

- 多 NVMe：通过 `NVME_GET_NSID` 从 `ndaN` 反查控制器，不再假设 `ndaN → nvmeN`。
- 每 30 秒探测一次并缓存，避免频繁 ioctl。
- 解析器直接读二进制页：`src/common/fmon_smart.c`。
- `nvmecontrol` 是 base 回退；`dmidecode`/`smartmontools`/`libdrm` 由安装脚本自动检测安装，系统已有则跳过；可用 `--skip-deps` 禁用。

## 4. 全硬件通用的真实含义

“全硬件通用”应理解为：

- 数据流、ABI、GUI、错误处理对任何 FreeBSD 设备一致；
- 有标准内核接口的传感器一定读取真实值；
- 没有标准接口的设备显示不可用；
- 不因为缺少某个厂商工具而错误地显示 0。

不能承诺“每一块 RAID 卡、每一代 GPU、每一台 BMC 都有温度和负载”：
这些通常只有厂商私有 ioctl/库，且 FreeBSD 支持程度不同。

## 5. 下游分发建议

1. 按 FreeBSD 版本/架构在目标环境构建（ABI 23）。
2. 不把 `smartctl`/`nvmecontrol` 作为编译硬依赖；安装脚本自动补齐运行时工具，内核 provider 优先，工具仅在失败时回退。
3. 在目标机上跑 `meson test -C build` 和页面冒烟。
4. 对没有传感器的机器，确认界面显示 “—” 而不是 0。
5. 大规模机器注意硬上限：CPU 64、磁盘 8、网卡 8、GPU 4、挂载点 16。
