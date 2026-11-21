fmon 1.3.1 - FreeBSD 安装包（保留 1.1.1 / 1.2.0 / 1.3.0）

新版本：
  fmon-1.3.1.pkg           FreeBSD 15 amd64 原生 pkg
  fmon-1.3.1-src.tar.gz    完整源代码
  RELEASE_NOTES_1.3.1.md   1.3.1 变更说明

历史版本（保留，不覆盖）：
  fmon-1.3.0.pkg / fmon-1.3.0-src.tar.gz
  fmon-1.2.0.pkg / fmon-1.2.0-src.tar.gz
  fmon-1.1.1.pkg / fmon-1.1.1-src.tar.gz

1.3.1 主要变化：
  - 网卡型号字段从 96 字节扩到 192 字节，完整显示 RTL8852BE /
    RTL8111 一类型号，不再出现 “Gigabit Ethernet C” 截断；
  - 共享内存 ABI 23 -> 24，daemon 与 GUI 必须一起升级；
  - 网络页型号改为标题下整行显示，长型号换行，不省略；
  - README 更新为四张 1.3.0/1.3.1 当前界面截图。

安装：
  sudo pkg install -f ./fmon-1.3.1.pkg
  sudo sysrc fmon_daemon_enable=YES
  sudo service fmon_daemon restart
  fmon-gui

卸载：
  sudo service fmon_daemon stop
  sudo sysrc fmon_daemon_enable=NO
  sudo pkg delete fmon

依赖（pkg 自动检查）：
  gtk4, cairo, sqlite3, libdrm

源代码编译：
  tar xzf fmon-1.3.1-src.tar.gz
  cd fmon-1.3.1
  meson setup build
  ninja -C build
  meson test -C build
  sudo ./install.sh -y
