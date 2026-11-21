fmon 1.3.0 - FreeBSD 安装包（保留 1.1.1 / 1.2.0）

新版本：
  fmon-1.3.0.pkg           FreeBSD 15 amd64 原生 pkg
  fmon-1.3.0-src.tar.gz    完整源代码
  RELEASE_NOTES_1.3.0.md   1.3.0 变更说明

历史版本（保留，不覆盖）：
  fmon-1.2.0.pkg / fmon-1.2.0-src.tar.gz
  fmon-1.1.1.pkg / fmon-1.1.1-src.tar.gz

1.3.0 主要变化：
  - 中英双语界面，运行时可切换；
  - 窗口大小记忆、X11 启动居中、退出响应加快；
  - 无 zpool / 缺失指标显示“—”，不伪造 0；
  - 页面顶部标签、快捷操作直出、深色背景与浅色复选框修复；
  - 进程页右键/崩溃修复，C11 模块化，20/20 测试通过。

安装：
  sudo pkg install ./fmon-1.3.0.pkg
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
  tar xzf fmon-1.3.0-src.tar.gz
  cd fmon-1.3.0
  meson setup build
  ninja -C build
  meson test -C build
  sudo ./install.sh -y
