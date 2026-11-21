fmon 1.3.2 - FreeBSD 安装包（保留 1.1.1 / 1.2.0 / 1.3.0 / 1.3.1）

新版本：
  fmon-1.3.2.pkg           FreeBSD 15 amd64 原生 pkg
  fmon-1.3.2-src.tar.gz    完整源代码
  RELEASE_NOTES_1.3.2.md   1.3.2 变更说明

历史版本（保留，不覆盖）：
  fmon-1.3.1.pkg / fmon-1.3.1-src.tar.gz
  fmon-1.3.0.pkg / fmon-1.3.0-src.tar.gz
  fmon-1.2.0.pkg / fmon-1.2.0-src.tar.gz
  fmon-1.1.1.pkg / fmon-1.1.1-src.tar.gz

1.3.2 主要变化：
  - 设备历史下拉框只列出 daemon 当前快照中的磁盘；
  - 外置盘拔掉后 2 秒内从下拉框消失，不再读取 SQLite 旧设备名；
  - 当前选中的盘被拔掉时自动切换到仍在线的磁盘；
  - 不按 ada/da/nda 白名单筛选，支持内核 devstat 枚举到的各类块设备；
  - 历史数据只对当前在线磁盘持续记录；
  - 共享内存 ABI 保持 24。

安装：
  sudo service fmon_daemon stop
  sudo pkg install -f ./fmon-1.3.2.pkg
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
  tar xzf fmon-1.3.2-src.tar.gz
  cd fmon-1.3.2
  meson setup build
  ninja -C build
  meson test -C build
  sudo ./install.sh -y
