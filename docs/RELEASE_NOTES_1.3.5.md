# fmon 1.3.5 Release Notes

Release date: 2026-11-22

## 版本定位

1.3.5 完成 1.3.4 审核遗留的 daemon 锁内 I/O 与磁盘历史过渡桶修复。

共享内存 ABI 保持 **24**，无数据结构变更。

## 修复

### D-03：磁盘元数据/SMART 移出 IO 写锁

- `thread_io` 先在锁外把 devstat、磁盘元数据、SMART 采样写入临时缓冲。
- 锁内只复制 `disk_count`、`disks[]`、磁盘读写速率和累计操作数。
- 首次探测或 60 秒元数据刷新时，`geom`/`diskinfo`/`smartctl`/`nvmecontrol` 不再阻塞其他采集线程。

### D-04：swap 回退移出 storage 写锁

- `SwapSample` 在锁外完成 KVM/swapinfo 探测。
- 锁内只写 swap 总量、已用量、百分比和 `probe_valid`。
- `swapinfo` 超时或失败也不会阻塞挂载点快照。

### H-01：删除磁盘历史过渡桶

- `disk_history_store` 在每次采样时标记当前设备。
- 拔出设备立即从本分钟累加器移除，不再写出旧累计值。
- flush 时只写本分钟实际采样过的设备。

## 验证

```text
构建：0 warning
单元测试：20/20
scan-build：0 条
fmon-gui：1.3.5（共享内存 ABI 24）
```

1.3.4 已完成的 D-01/D-02/C-01/C-02 测试继续有效：

- FIFO 慢历史 I/O：15 秒 / 150 样本快照心跳 0 gaps；
- fake zpool list 睡眠 3 秒：15 秒 / 150 样本 0 gaps；
- getpeereid 失败控制请求返回 ERR，正常路径 PONG。

## 仍待处理

- 真正的 root 级 `mdconfig` 慢盘、`zpool suspend` 和非 operator 用户测试；
- 更长时间的 ASAN GUI soak；
- D-03/D-04 的系统级故障注入（当前只做了代码复审与静态分析）。

## 升级

```sh
sudo service fmon_daemon stop
sudo pkg install -f ./fmon-1.3.5.pkg
sudo service fmon_daemon start
fmon-gui
```

历史版本（1.3.4 及更早）只通过 GitHub Releases 访问。

## 包

- `fmon-1.3.5.pkg`
- `fmon-1.3.5-src.tar.gz`
