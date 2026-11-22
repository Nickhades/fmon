# fmon 1.3.6 Release Notes

Release date: 2026-11-22

## 版本定位

1.3.6 完成 CSV 导出文本字段统一转义与公式注入加固，并补充安全模型文档和 CSV 单元测试。

共享内存 ABI 保持 **24**，无数据结构变更。

## 修复

### CSV 统一转义

- 新增 `src/gui/support/csv_util.c`。
- 进程导出的 User、System、State、Command 全部使用同一转义函数。
- 逗号、双引号、换行按 RFC 4180 规则处理。
- 以 `=`、`+`、`-`、`@`、制表符、回车开头的文本增加 apostrophe 前缀，降低表格公式注入风险。
- 新增 `test_csv`；单元测试从 20 项增加到 21 项。

### 安全文档

新增 `docs/SECURITY.md`：

- daemon/GUI 权限模型；
- 共享内存 0644 读取边界；
- 控制套接字 `0660 root:operator`（无 operator 时 wheel）；
- operator/wheel 成员可执行的敏感操作；
- root 级测试注意事项；
- 建议多用户环境使用专用 `fmon` 组。

## 验证

```text
构建：0 warning
单元测试：21/21
scan-build：0 条
fmon-gui：1.3.6（共享内存 ABI 24）
```

test_csv 覆盖：

- 普通文本、空字段；
- 逗号、双引号、换行；
- `= + - @` 公式前缀。

## 仍待处理

- root 级 `mdconfig` 慢盘、`zpool suspend`、非 operator 用户实测；
- 更长时间的 ASAN GUI soak；
- GUI 对话框和控制通道端到端自动化。

## 升级

```sh
sudo service fmon_daemon stop
sudo pkg install -f ./fmon-1.3.6.pkg
sudo service fmon_daemon start
fmon-gui
```

历史版本（1.3.5 及更早）只通过 GitHub Releases 访问。

## 包

- `fmon-1.3.6.pkg`
- `fmon-1.3.6-src.tar.gz`
