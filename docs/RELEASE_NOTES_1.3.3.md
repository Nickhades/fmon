# fmon 1.3.3 Release Notes

Release date: 2026-11-22

## 版本定位

1.3.3 修复 1.3.2 代码审核中发现的日志上下文 use-after-free，并清理静态分析告警。

共享内存 ABI 保持 **24**，无数据结构变更。

## 修复

### 日志上下文 use-after-free

- `src/gui/views/log_context.c` 在点击日志行、弹出上下文时，
  提前 `g_free(line)` 后又把 `line` 传给 `log_render_raw_context()`
  和 `g_strdup()`。
- 现在 `line` 会一直保留到缓存命中、异步抓取或普通显示分支结束时再释放。
- 影响：日志页点击非 PID 行不再存在释放后使用风险。

### 静态分析清零

`scan-build` 在 1.3.2 上报告 5 条诊断；修复后为 **0 条**：

- 日志上下文 UAF：真实缺陷，已修复；
- 服务列表 `names` 空指针：加防御性检查；
- daemon 静态信息填充：加 `shm == NULL` 防御；
- GPU 驱动版本探测：改用 `read(2)`，移除 stdio EOF/错误状态歧义。

## 验证

```text
构建：0 warning
单元测试：20/20
scan-build：No bugs found
fmon-gui：1.3.3（共享内存 ABI 24）
```

## 升级

```sh
sudo service fmon_daemon stop
sudo pkg install -f ./fmon-1.3.3.pkg
sudo service fmon_daemon start
fmon-gui
```

历史版本（1.3.2 及更早）只通过 GitHub Releases 访问，不再放在 main 分支的 dist/ 中。

## 包

- `fmon-1.3.3.pkg`
- `fmon-1.3.3-src.tar.gz`
