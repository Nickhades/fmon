fmon 1.1.1 - FreeBSD 安装包

文件：
  fmon-1.1.1.pkg           FreeBSD pkg 安装包（FreeBSD 15 amd64）
  fmon-1.1.1-src.tar.gz    完整源代码

安装：
  sudo pkg add ./fmon-1.1.1.pkg
  sudo sysrc fmon_daemon_enable=YES
  sudo service fmon_daemon start
  fmon-gui

卸载：
  sudo service fmon_daemon stop
  sudo sysrc fmon_daemon_enable=NO
  sudo pkg delete fmon

依赖（pkg 会自动检查）：
  gtk4 4.20.4_1
  cairo 1.18.2,3
  sqlite3 3.54.1,1
  libdrm 2.4.134,1

源代码编译：
  tar xzf fmon-1.1.1-src.tar.gz
  cd fmon-1.1.1
  meson setup build
  ninja -C build
  meson test -C build
  sudo ./install.sh
