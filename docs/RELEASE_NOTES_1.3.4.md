# fmon 1.3.4 Release Notes

Release date: 2026-11-22

## 版本定位

1.3.4 是 1.3.3 之后的 daemon 稳定性与安全加固补丁，修复代码审核中 D-01/D-02/C-01/C-02。

共享内存 ABI 保持 **24**，无数据结构变更。

## 修复

### D-01：历史落盘移出快照写锁

- 新增独立 `thread_history`，每 10 秒用 `fmon_shm_snapshot()` 取得一致快照。
- raw/1m/5m/1h/GPU/ZFS 文件写入不再持有 seqlock 写事务。
- 慢磁盘只会延迟历史线程，不会阻塞 CPU/IO/网络/发布线程。

### D-02：zpool 容量探测移出写锁

- `zpool list` 改为在锁外执行，锁内只合并容量字段和已有 I/O 计数。
- `zpool list` 卡住或超时时，publisher 仍按 1 Hz 更新快照。

### C-01：控制通道 fail-closed

- `getpeereid()` 失败时立即返回 ERR，不再在无对端凭据时继续处理 root 命令。
- 正常 operator/wheel 路径保持不变。

### C-02：删除重复套接字路径写入

- `fmon_ctl_start()` 中重复的 `snprintf(g_sock_path, ...)` 已删除。

## 验证

```text
构建：0 warning
单元测试：20/20
scan-build：0 条
ASAN 单元测试：20/20
ASAN GUI soak：120 秒页面切换，无 AddressSanitizer 报错
```

针对性测试：

- **D-01**：history 文件替换为 FIFO 并阻塞 `fopen`；15 秒 / 150 样本快照心跳 **0 gaps**（阈值 1.5 秒）。
- **D-02**：fake `zpool` 的 `list` 睡眠 3 秒、`iostat` 睡眠 2 秒；调用确实发生，15 秒 / 150 样本 **0 gaps**。
- **C-01**：LD_PRELOAD 覆盖 `getpeereid()` 返回失败，客户端收到 `ERR 无法获取对端凭据`；正常路径 `OK pong`。

限制：当前环境没有 root，未执行真实的 `mdconfig` 慢盘、`zpool suspend` 和非 operator 用户测试；使用 FIFO/fake zpool 覆盖同一阻塞路径。

## 仍待处理

- D-03：磁盘元数据/SMART 探测仍在 IO 写锁内。
- D-04：swap 的 `swapinfo` 回退仍在 storage 写锁内。
- H-01：磁盘拔出后的最后一个历史桶。

## 升级

```sh
sudo service fmon_daemon stop
sudo pkg install -f ./fmon-1.3.4.pkg
sudo service fmon_daemon start
fmon-gui
```

历史版本（1.3.3 及更早）只通过 GitHub Releases 访问。

## 包

- `fmon-1.3.4.pkg`
- `fmon-1.3.4-src.tar.gz`
