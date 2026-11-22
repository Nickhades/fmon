fmon 1.3.4 - FreeBSD 安装包

当前版本：
  fmon-1.3.4.pkg           FreeBSD 15 amd64 原生 pkg
  fmon-1.3.4-src.tar.gz    完整源代码
  RELEASE_NOTES_1.3.4.md   1.3.4 变更说明

历史版本（1.3.3 及更早）只通过 GitHub Releases 历史页访问：
  https://github.com/Nickhades/fmon/releases

安装：
  sudo service fmon_daemon stop
  sudo pkg install -f ./fmon-1.3.4.pkg
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
  tar xzf fmon-1.3.4-src.tar.gz
  cd fmon-1.3.4
  meson setup build
  ninja -C build
  meson test -C build
  sudo ./install.sh -y
