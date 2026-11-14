# C11 代码审核（fmon4）

审核日期：2026-11-11  编译器：clang 19.1.7 / FreeBSD 15.1

## 编译配置

- 语言标准：`c_std=c11`
- 警告：`warning_level=3` 额外开启
  `-Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wpointer-arith -Wwrite-strings -Wvla`
- 加固：`-D_FORTIFY_SOURCE=2 -fstack-protector-strong`，
  链接 `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack`

## 审核结果

| 项目 | 结果 |
|---|---|
| 编译警告 | **0**（GUI + daemon + tests） |
| 单元测试 | 6/6 通过（shm / child / ps 解析 / rc.conf 解析 / 内存说明文案 / 内存队列占比） |
| 头文件保护 | 全部 `.h` 均有 `#ifndef/#define/#endif` |
| 危险函数 | 无 `strcpy/sprintf/gets/atoi/rand` 调用（仅注释提及） |
| 模块内状态 | 新拆模块统一 `static`，接口经头文件暴露 |
| 数据类型 | 共享内存结构使用固定宽度整数（`uint32_t/uint64_t`），`double` 用于比例/速率 |
| 错误处理 | 子进程走 `fmon_child_run()` 带超时；sysctl/shm 打开失败均返回错误串，不静默 |
| 并发 | 共享内存写者持有写锁；读侧使用 seqlock 快照，避免撕裂 |
| 命名 | 统一 `gui_*` / `fmon_*` 前缀；视图构造函数已改为 `gui_*_view_new`（第 6 轮完成） |
| 魔数 | 时间常量集中在 `fmon_shm.h`（`FMON_SAMPLE_INTERVAL_MS` 等）；绘图内少量像素常量属局部实现细节 |

## 已知待办（不阻塞）

1. `main.c` 作为应用外壳保留：命令行、窗口组装、1 秒刷新调度与退出处理
   （1597 行，占 GUI 代码约 13%）；各页面/功能已全部独立成模块；
2. ~~旧概览卡片页（`sys/hw/zfs`）不可达代码~~：已删除，指标曲线改由
   `gui_pages.c` 的注册表统一提供（第 7 轮完成）；
3. 旧视图模块函数名（`create_process_view` 等）与 `gui_*` 前缀不一致，
   可在不改行为的前提下批量重命名；
4. ~~`gui_upd_gpu` 与旧 GPU 详情路径~~：已删除（第 5 轮）。

## 重构后规模

| 项目 | 重构前 | 重构后 |
|---|---|---|
| `main.c` | 4237 行 | 1597 行 |
| GUI 源文件数 | 12 | 27 |
| daemon 采集器 | 8 | 8（+collectors.h 统一入口）|
| 编译警告 | 0 | 0 |
| 测试 | 2/2 | 6/6（新增 ps、rc.conf 解析器、内存说明文案、内存队列占比单测） |

## 结论

当前代码满足 C11 编译与严格警告要求，未发现未定义行为级别的明显缺陷；
剩余工作是结构性清理（窗口拆分、死代码删除、命名统一），不影响功能与稳定性。
