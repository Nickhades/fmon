# fmon 安全模型与权限边界

本文说明 fmon 的本地权限边界，重点是多用户 FreeBSD 桌面上的控制通道。

## 1. 进程模型

- `fmon-daemon`：以 root 运行，读取 kvm/sysctl/devstat/CAM/NVMe，执行受控的特权操作。
- `fmon-gui`：以登录用户运行，只读映射共享内存，不直接打开特权设备。
- 控制通道：本地 Unix 域套接字，默认 `/var/run/fmon-ctl.sock`；本地调试时退回 `/tmp/fmon-ctl.sock`。

## 2. 共享内存

- 名称：`/fmon_daemon_shm_v24`（ABI 24）。
- 权限：`0644`，root 写，普通用户只读。
- 头部校验：magic、ABI、struct_size。
- 读取使用 seqlock 快照；旧版 0666 可写共享内存的问题已移除。

共享内存可被本机任意用户读取，包含进程名、网络地址等监控信息。如果机器是多用户高敏感环境，应改用按组 ACL，或限制登录用户。

## 3. 控制通道权限

套接字文件：

```text
0660 root:operator
```

如果系统没有 `operator` 组，则退回 `wheel`。守护进程还会用 `getpeereid()` 和补充组检查对端凭据；认证失败时 fail-closed。

### 这意味着什么

加入 `operator` 或 `wheel` 的本地用户可以请求：

- 向任意 PID 发送 TERM/KILL/STOP/CONT/HUP；
- 启停/重启服务；
- 修改 sysrc 的 `*_enable`；
- 修改 nice/cpuset；
- 添加/删除 rctl 规则。

这是“可信本地管理员组”边界，不是普通用户边界。不要把不可信账号加入这些组。

### 建议

- 多用户环境单独创建 `fmon` 组，而不是直接使用 `wheel`；
- 通过 `pw groupmod` 只把需要管理监控的账号加入；
- 定期检查 `/var/run/fmon-ctl.sock` 的属组和权限；
- 不要把控制套接字暴露到网络命名空间或非本地路径。

## 4. 命令执行

- 采集和控制命令通过 `posix_spawnp()` + argv 执行，不拼接 shell。
- 服务名走 token 白名单；pid 拒绝 0/1 和自身；信号有白名单；CPU 列表和 rctl 规则有字符集校验。
- 日志页保留固定 shell 脚本入口：日志源命令是常量；日志上下文脚本把日志行作为位置参数传入，不拼进脚本。
- CSV 导出统一转义文本字段，并对 `= + - @` 开头的字段加 apostrophe 前缀，降低表格公式注入风险。

## 5. 根级测试建议

以下测试需要 root，普通会话无法安全执行：

```sh
# 控制通道：非 operator/wheel 用户
su nobody -c '/tmp/fmon-ctl-client /var/run/fmon-ctl.sock'

# 慢盘：单独准备测试池/挂载，不要动生产历史目录
# zpool suspend <test-pool> 仅用于隔离测试池
```

生产环境不要对真实池执行 `zpool suspend`；慢盘/故障注入应使用临时 vnode 盘或专用测试池。
