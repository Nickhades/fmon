# fmon 1.3.7 Release Notes

Release date: 2026-11-23

## 版本定位

1.3.7 在「关于」对话框中明确显示软件版本号，并与共享内存 ABI 分开标注。

共享内存 ABI 保持 **24**，无数据结构变更。

## 界面

「关于」对话框现在显示：

```text
软件版本 1.3.7 · 共享内存 ABI 24
```

不再只写笼统的“版本”，便于确认当前安装的 fmon 软件版本和 ABI。

## 验证

```text
构建：0 warning
单元测试：21/21
fmon-gui：1.3.7（共享内存 ABI 24）
```

## 升级

```sh
sudo service fmon_daemon stop
sudo pkg install -f ./fmon-1.3.7.pkg
sudo service fmon_daemon start
fmon-gui
```

历史版本（1.3.6 及更早）只通过 GitHub Releases 访问。

## 包

- `fmon-1.3.7.pkg`
- `fmon-1.3.7-src.tar.gz`
