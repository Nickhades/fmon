# fmon 1.3.1 Release Notes

Release date: 2026-11-22

## 版本定位

1.3.1 是 1.3.0 的补丁版本，修复网络页网卡硬件型号被截断的问题。

共享内存 ABI 从 **23 升到 24**：`FmonShmData.nic_models` 的单条型号从 96 字节扩大到 192 字节。

**daemon 与 GUI 必须一起升级**，否则新版 GUI 会因 ABI/结构大小不匹配拒绝旧共享内存。

## 修复

### 网卡型号完整显示

- 旧版 `nic_models[4][96]` 在 96 字符处截断，长型号显示为：
  - `... Gigabit Ethernet C`
- 采集器同时有 `len < 88` 的二次截断。
- 现在：
  - 每条型号 192 字节；
  - 设备名 160 字节；
  - 页面把型号放在标题下整行显示，不省略、不裁切；
  - 型号过长时换行显示，tooltip 保留完整文本。

### 截图

- 删除旧的 `docs/screenshot.png`。
- 新增四张 1.3.0/1.3.1 当前界面截图：
  - `docs/screenshots/01-cpu.png`
  - `docs/screenshots/02-process.png`
  - `docs/screenshots/03-zfs.png`
  - `docs/screenshots/04-history.png`
- README 中英文均更新为四图展示。

## 升级

```sh
sudo pkg install -f ./fmon-1.3.1.pkg
sudo service fmon_daemon restart
fmon-gui
```

旧包保留：

- `fmon-1.3.0.pkg`
- `fmon-1.3.0-src.tar.gz`

## 包

- `fmon-1.3.1.pkg`
- `fmon-1.3.1-src.tar.gz`

## 已知边界

- 1.3.0 的 daemon 不能与 1.3.1 GUI 混用（ABI 24）。
- Wayland 仍由 compositor 决定窗口位置。
