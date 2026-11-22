# fmon 1.3.2 Release Notes

Release date: 2026-11-22

## 版本定位

1.3.2 修复存储页“设备历史”仍然显示已拔出外置磁盘的问题。

共享内存 ABI 保持 **24**；daemon 与 GUI 仍应同时升级。

## 修复

### 设备历史只列出当前磁盘

- 旧实现从 SQLite 历史库读取“曾经出现过的设备名”，因此：
  - 外置盘拔掉后仍留在下拉框；
  - 历史库里的旧设备会一直显示。
- 现在下拉框只读取 daemon 当前快照的 `disks[]`：
  - 外置盘插入后自动出现；
  - 拔出后 2 秒内从下拉框消失；
  - 如果当前选中的盘被拔出，自动切换到仍在线的磁盘；
  - 没有磁盘时显示“未检测到磁盘”。

### 设备列表通用化

- 不再按 `ada` / `da` / `nda` 等名字白名单筛选。
- 直接使用 devstat 枚举到的当前块设备，IDE/SATA/SAS/NVMe/USB/SCSI 等均由内核枚举结果决定。
- 不因接口类型或型号未知而隐藏磁盘。

### 历史数据

- 只对当前快照中的磁盘持续写入历史。
- 磁盘重新插入后继续记录；旧历史点保留在 SQLite 中，但未插入时不会出现在设备选择器。
- 设备历史仍为每分钟聚合、保留 7 天。

## 升级

```sh
sudo service fmon_daemon stop
sudo pkg install -f ./fmon-1.3.2.pkg
sudo service fmon_daemon start
fmon-gui
```

历史版本（1.3.1 及更早）只通过 GitHub Releases 访问，不再保留在 main 分支的 dist/ 中。

## 包

- `fmon-1.3.2.pkg`
- `fmon-1.3.2-src.tar.gz`
