# 网络监控配置与兼容性（原生网卡 / wifibox）

本文说明 fmon4 如何识别网络、在 FreeBSD 原生网卡与 wifibox（bhyve + Alpine）环境下分别需要什么配置，以及活动连接各字段的来源和限制。

## 1. 自动识别，不绑定网卡名

- 接口列表来自 `getifaddrs(3)`，枚举系统当前所有 IPv4/IPv6 接口；
- 连接表来自 `sockstat -4 -6`，同时覆盖 tcp4/tcp6/udp4/udp6；
- 代码里**没有**写死 `em0`、`wlan0`、`vm-public` 等名字；
- 因此：

| 环境 | 需要做什么 |
|---|---|
| FreeBSD 原生支持的网卡 | 无，启动后自动出现 |
| wifibox（无线由 Alpine 虚拟机托管） | 无，宿主侧看到的网桥/tap 会被自动枚举 |
| USB 网卡 / 多个网卡 / VPN | 无，全部列出 |

## 2. wifibox 环境

wifibox 通常在宿主侧创建：

- 一个网桥（常见 `vm-public` 或 `bridge0`）
- 一个 tap 设备（`tap0` 等）
- 宿主默认路由指向该网桥

fmon4 只做只读采集，不参与转发，因此**不需要修改 wifibox 配置**。

启动顺序建议：

```
wifibox 服务先启动 → 宿主拿到地址 → 再启动 fmon-gui
```

如果先启动 GUI，接口列表会在下一次刷新时自动补上；连接表刷新按钮也可以手动触发。

排查命令：

```sh
ifconfig -l
ifconfig vm-public
netstat -rn
sockstat -4 -6 | head
```

只要 `ifconfig -l` 能看到网桥/tap，界面就能显示接口和连接。

## 3. 活动连接字段来源

| 列 | 来源 |
|---|---|
| 用户 / UID | sockstat + getpwnam |
| 命令 / PID / FD | sockstat |
| 协议 | sockstat（tcp4/tcp6/udp4/udp6） |
| 状态 | netstat -an 的 ESTABLISHED/LISTEN/TIME_WAIT/...，按五元组匹配 |
| 服务 | getservbyport(3)（如 https、domain） |
| 本地 / 远端地址 | sockstat |
| 接口 | getifaddrs 按本地地址匹配（lo0 / wlan0 / vm-public ...） |
| 持续 | GUI 首次看到该连接的时间，不是内核 socket 建立时间 |

## 4. 每连接收发字节 / 实时速率

**FreeBSD 的 sockstat、netstat 不提供按 socket 的累计字节数**；`netstat -b` 只统计接口。

可选实现路径（本项目尚未实现）：

1. **DTrace**：挂 `sockfd` 发送/接收路径按五元组聚合，开销可控但需要 root 和 dtrace 权限；
2. **BPF/pcap**：抓包按五元组哈希，能拿到精确字节和速率，但高流量下 CPU/内存开销大；
3. **内核计数器**：需要改内核或驱动，不适合分发。

当前界面不伪造字节数；接口级别的收发统计在“网络概览”页，是准确的。

## 5. 反向 DNS / GeoIP

- 反向 DNS：需要对每个远端 IP 调 `getnameinfo`，必须放到工作线程并缓存，否则会卡住界面；
- GeoIP：需要离线库（MaxMind GeoLite2 等）和定期更新；
- 两者都未实现，当前显示数字地址。

## 6. 权限

- `sockstat` 非 root 只能看到当前用户有权限的 socket；
- 若分发后需要看到**所有用户**的连接，需要：
  - 以 root 运行 GUI，或
  - 由 root daemon 采集后通过共享内存下发（当前未实现）。
- 接口列表不要求 root。

## 7. 排序与操作

- 列头可点击排序：用户、UID、命令、PID、协议、状态、服务、地址、接口、FD、持续；
- 没有每连接字节，因此暂时不能“按流量排序”；
- 右键 Kill / traceroute 尚未接入。

## 8. 分发检查清单

- [ ] 系统有 `/usr/bin/sockstat`、`/usr/sbin/netstat`
- [ ] 原生网卡或 wifibox 网桥已拿到 IP
- [ ] 如需全部用户连接：以 root 运行或等待 daemon 采集支持
- [ ] 语言环境可显示中文（可选）
