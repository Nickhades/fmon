fmon 1.2.0 - FreeBSD 安装包（保留 1.1.1）

文件：
  fmon-1.2.0.pkg           FreeBSD 15 amd64 原生 pkg（新）
  fmon-1.2.0-src.tar.gz    完整源代码（新）
  fmon-1.1.1.pkg           旧版安装包（保留，未覆盖）
  fmon-1.1.1-src.tar.gz    旧版源码（保留，未覆盖）
  RELEASE_NOTES_1.2.0.md   1.2.0 变更说明

1.2.0 主要变化：
  - 内核接口优先的采集契约：无接口显示“—”，绝不伪造 0；
  - NVMe/ATA/SCSI 磁盘温度直接走内核/CAM，nvmecontrol/smartctl 仅回退；
  - 磁盘元数据内核化（DIOCG* / NVMe controller / CAM），删除猜盘型；
  - ABI 23 有效位：Swap/zpool/内存条/挂载点/进程/ZFS/电池区分真实 0 与失败；
  - 安装脚本自动检测/安装缺失的 dmidecode、smartmontools、libdrm；
  - 自绘 SVG 应用图标、菜单图标与 .desktop，不依赖系统图标主题；
  - 旧版截图九宫格移除，README 只保留一张 1.2.0 界面截图。

安装：
  sudo pkg install ./fmon-1.2.0.pkg
  sudo sysrc fmon_daemon_enable=YES
  sudo service fmon_daemon start
  fmon-gui

卸载：
  sudo service fmon_daemon stop
  sudo sysrc fmon_daemon_enable=NO
  sudo pkg delete fmon

依赖（pkg 自动检查）：
  gtk4, cairo, sqlite3, libdrm

源代码编译：
  tar xzf fmon-1.2.0-src.tar.gz
  cd fmon-1.2.0
  meson setup build
  ninja -C build
  meson test -C build
  sudo ./install.sh -y
