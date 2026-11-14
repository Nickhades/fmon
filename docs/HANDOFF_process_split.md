# 交接：process_view.c / service_view.c 细化拆分（已完成）

> 状态：**已完成**（2026-11-11）。原 1600 行的 `process_view.c` 与 965 行的
> `service_view.c` 已按 4 步拆分并逐步骤验证。本文件保留模块地图与验证结论，
> 供后续维护参考。

## 1. 当前结构

```
src/gui/views/
├── process_internal.h    148 行   ProcItem / ProcPage / PsRow / SignalOp 等内部类型
├── process_model.c       211 行   ProcItem、ps 解析、过滤、排序
├── process_windows.c     232 行   进程详情、lsof / sockstat 文本窗口
├── process_actions.c     456 行   信号 / renice、右键菜单、行激活
├── process_list.c        169 行   模型栈、ColumnView 列、单元格工厂
├── process_view.c        578 行   工具栏、刷新调度、选择保持、CSV 导出
├── service_internal.h     99 行   ServicePage / SvcOp / RowAction 等
├── service_model.c       202 行   rc.conf 解析、rc.d 脚本扫描
├── service_actions.c     320 行   启停 / 自启操作、服务详情窗口
├── service_list.c        231 行   服务行构建、列表重建、搜索过滤
└── service_view.c        211 行   工具栏、notebook、service -e 刷新调度
```

依赖方向（禁止反向）：

```
process_view.c    → process_list.c / process_actions.c / process_windows.c
process_list.c    → process_actions.c
process_actions.c → process_windows.c / process_model.c
process_windows.c → process_model.c

service_view.c    → service_list.c / service_actions.c
service_list.c    → service_actions.c
service_actions.c → service_model.c
```

唯一的上行调用是操作完成后的刷新：

- `service_actions.c` 在操作成功后调用 `svc_page_refresh()`（`service_view.c` 实现），
  保持“谁持有 page 谁负责刷新”的所有权；
- 进程页不需要该调用（3 秒自动刷新 + 手动刷新已覆盖）。

公开头文件只暴露 `gui_process_view_new()` / `gui_service_view_new()`；
`*_internal.h` 仅供对应 `process_*.c` / `service_*.c` 使用。

## 2. 拆分步骤回顾

| 步骤 | 内容 | 验证 |
|---|---|---|
| 1 | 新建 `process_internal.h` / `service_internal.h`，集中类型 | 编译 0 warning、两页冒烟 |
| 2 | 抽数据层 `*_model.c`（解析 / 过滤 / 排序） | 编译、`meson test` 2/2、冒烟 |
| 3 | 抽操作层 `*_actions.c`（信号 / 菜单 / 启停），查看窗口在后续步骤拆到 `process_windows.c` | 编译、冒烟（右键菜单、详情窗口） |
| 4 | 抽列表构建 `*_list.c`（模型栈 / 行 / 过滤） | 编译、`meson test` 2/2、冒烟 |

每一步都遵循“只移动一组函数 → 立即编译 → 冒烟”，没有用脚本大段切割源码；
移动时用“整段精确匹配删除 + 新文件写入”的方式，避免误删 `on_ps_done` 一类的回调。

## 3. 顺带修复

- **刷新后选中行丢失**：`on_ps_done` 重建 `GListStore` 前记住选中进程的 pid，
  重建后按 pid 恢复；进程已退出时回退为空选择，并同步刷新底部按钮的可用状态。
  实测：选中一行 → 新建进程 → 手动刷新 → 选中行保持。

## 4. 验证结果

```sh
ninja -C build        # 全量重建，0 warning
meson test -C build   # 2/2 OK
./build/src/gui/fmon-gui --page proc   # 列表 / 搜索 / 右键菜单 / 详情 / 刷新
./build/src/gui/fmon-gui --page svc    # 两个标签页 / 详情窗口 / 计数
```

冒烟通过截图核对（进程页列表与提示、右键菜单 10 项、进程详情窗口、
服务页两个标签页与计数、服务详情窗口）。

## 5. 约定（后续修改仍需遵守）

- C11；`warning_level=3` 已开 `-Wshadow -Wstrict-prototypes
  -Wmissing-prototypes -Wpointer-arith -Wwrite-strings -Wvla`，提交必须 0 warning；
- 4 空格缩进，行宽尽量 ≤ 80；
- 模块内符号 `static`；跨文件接口进 `*_internal.h` 并加前缀
  （`proc_*` / `svc_*`），避免 C 无命名空间带来的冲突；
- 外部命令一律走 `fmon_gui_run_async*`，不在 GTK 主线程同步执行；
- 重建列表时注意保持 selection，不销毁正在被选中的控件。

## 6. 后续可选

- 查看类窗口已拆到 `process_windows.c`（232 行），`process_actions.c` 降到 456 行；
  如需进一步细化，可把右键菜单的 10 个回调按“只读 / 有副作用”再分组；
- `service_model.c` 目前只有解析与扫描，可把 `service -e` 输出解析也移入，
  为解析器补单元测试（README 中“解析器测试仍待补充”一条仍然成立）；
- `README.md` 第 313 行附近的目录树仍是拆分前的旧布局（`gui/` 平铺），
  与现在的 `app/charts/pages/views/support` 不一致，可另行更新。
