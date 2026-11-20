# fmon 1.2.0 Release Notes

Release date: 2026-11-20

## 版本定位

1.2.0 的重点不是增加更多“猜出来”的指标，而是把采集契约做严格：

> **内核接口优先 → base 工具回退 → 可选依赖回退 → 不可用显示“—”；
> 设备名不硬编码，任何情况下都不伪造数据。**

共享内存 ABI 从 22 升到 **23**，daemon 与 GUI 必须一起升级。

## 新增

### 采集可靠性

- 新增全局探测有效位 `probe_valid`：
  - Swap、zpool、内存条、挂载点、进程、ZFS ARC、电池分别标记成功/失败。
  - “真实 0”和“探测失败”不再共用同一个 0。
- 新增 `FmonDiskInfo.meta_valid` 与 `FmonIfaceInfo.detail_valid`。
- GUI 对不可用项显示“不可用 / —”，不再显示 0 冒充未知值。

### 磁盘温度与 SMART

- NVMe：直接使用 `NVME_GET_NSID` + `NVME_PASSTHROUGH_CMD` 读健康页。
- ATA/SATA：直接使用 libcam `CAM ATAIO` + SMART READ DATA。
- SCSI/SAS：直接使用 libcam `CAM SCSI LOG SENSE` page 0x0D。
- 多 NVMe：通过 `NVME_GET_NSID` 反查控制器，不再假设 `ndaN → nvmeN`。
- 仅在内核 provider 失败时才回退 base `nvmecontrol` 或可选 `smartctl`。
- 新增 `smart_source`，磁盘温度 tooltip 显示真实来源。
- 磁盘温度和寿命不再由普通用户 GUI 直接读取 `/dev/nvme0`。

### 磁盘元数据

- 容量/序列号：`DIOCGMEDIASIZE` / `DIOCGIDENT`。
- NVMe 型号/序列号：`NVME_GET_CONTROLLER_DATA`。
- ATA 型号/序列号/转速：CAM ATA IDENTIFY。
- SCSI/SAS 型号：CAM SCSI INQUIRY。
- SSD/HDD：ATA rotation rate 或 `kern.cam.<driver>.<unit>.rotating`。
- 失败才回退 `geom`/`diskinfo`。
- 删除了根据型号字符串/容量猜测 SSD/HDD/闪存的逻辑；无法确认时保持 UNKNOWN。

### 图表与历史

- 历史点使用真实时间戳 `history_time_us[]`，daemon 停摆留下真实缺口。
- 线性段渲染，NaN 断线，不补 0。
- 历史页 24h 峰值包络：平均线 + min/max 带，峰值不被桶平均抹掉。
- 百分比图默认 0–100，可选显式自动量程。
- 悬停吸附真实采样点。

### 安装与桌面

- 安装脚本自动检测运行工具：
  - `dmidecode`
  - `smartmontools`
  - `libdrm`
  - 已有则跳过，缺失自动安装；`--skip-deps` 可禁用。
- 检测 FreeBSD base 工具，缺失时列出并降级，不尝试用 pkg 覆盖基础系统。
- 检测 NVIDIA GPU；不盲目自动安装驱动，避免驱动版本冲突。
- 新增完全自绘的 SVG 应用图标与 `.desktop` 入口。
- 导航/菜单图标全部来自项目自己的 gresource，不依赖桌面图标主题。
- 安装/卸载会处理图标、desktop 文件和图标缓存。

## 变更

- 共享内存 ABI **23**（`/fmon_daemon_shm_v23`）。
- 升级后必须同时重启/重装 `fmon-daemon` 与 `fmon-gui`。
- 包文件名改为 `fmon-1.2.0.pkg`，源代码包改为 `fmon-1.2.0-src.tar.gz`。
- 挂载点容量改用 `getmntinfo()` 返回的 `statfs(2)` 数据，不再重复 `statvfs(3)` 调用。
- 包内新增：
  - `/usr/local/share/icons/hicolor/scalable/apps/org.freebsd.fmon.svg`
  - `/usr/local/share/applications/org.freebsd.fmon.desktop`
- 安装脚本不再把运行时工具当作“只提示”的可选项；缺失时默认补齐。

## 修复

- 普通用户 GUI 无法读取 `/dev/nvme0` 导致磁盘温度永远为 `—` 的问题。
- 多 NVMe 控制器下 `ndaN → nvmeN` 的错误映射假设。
- CAM rotating sysctl 使用了错误的 OID（`kern.cam.da0.rotating` 而不是 `kern.cam.da.0.rotating`）。
- zpool/内存条/挂载点/进程采集失败时显示 0 的歧义。
- 磁盘型号/容量推测造成 SSD/HDD 误判。
- 旧安装脚本重复/不完整处理运行时依赖的问题。

## 移除

- 从磁盘 SMART 首选路径移除对 `nvmecontrol` 的依赖。
- 从磁盘 SMART 首选路径移除对 `smartctl` 的依赖（只保留可选回退）。
- 移除“按型号字符串/容量猜盘型”的逻辑。
- 移除未使用的 `nvtop` 安装提示。
- 从 README/页面移除旧的九宫格截图和旧截图文件，只保留一张 1.2.0 界面截图。

## 已知边界

- NVIDIA `nvidia-smi` 属于驱动，安装脚本只检测并提示，不自动安装驱动。
- 没有标准温度接口的 RAID/虚拟盘会显示 `—`；这不是界面错误。
- SCSI/SAS 的 LOG SENSE 温度页支持因盘而异。
- Wi-Fi ioctl、网卡 PCIOCGETCONF、电池 GUI 去 `acpiconf` 等迁移仍在后续清单中。

## 安装/升级

```sh
sudo pkg install ./fmon-1.2.0.pkg
sudo sysrc fmon_daemon_enable=YES
sudo service fmon_daemon restart
fmon-gui
```

源码：

```sh
tar xzf fmon-1.2.0-src.tar.gz
cd fmon-1.2.0
meson setup build
ninja -C build
meson test -C build
sudo ./install.sh -y
```
