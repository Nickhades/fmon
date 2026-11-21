# UI 间距与视觉规范

## 8px 网格

所有页面外边距、卡片内边距、控件间距优先使用：

| 令牌 | 值 | 用途 |
|---|---:|---|
| FMON_SPACE_1 | 4 | 图标与文字 |
| FMON_SPACE_2 | 8 | 控件间距、表格行 |
| FMON_SPACE_3 | 12 | 卡片间距 |
| FMON_SPACE_4 | 16 | 页面/卡片内边距 |
| FMON_SPACE_5 | 24 | 页面外边距 |

## 当前状态

- .content-area：16 / 24 px
- .card：16 px
- .page-card：16 px
- .view-card：16 px
- 基础圆角：8 / 12 / 14 px

页面代码里的 gtk_widget_set_margin_* 和主要
gtk_box_new(..., spacing) 已替换为 FMON_SPACE_* 令牌；
剩余 0 间距为有意贴边。每改一页在 1304×760 和
1920×1052 截图回归。

## 终端视觉

默认主题：深色终端（新用户）；已有 light 配置仍保留。
CSS 实际由 `src/gui/app/gui_css.c` 统一加载（main.c 只启动应用）。

- 等宽字体：JetBrains Mono / DejaVu Sans Mono / Noto Sans Mono CJK / monospace
- 数据线：纯色、1.0–1.5px、直角阶跃
- 图表：无渐变、无面积填充
- 卡片/面板：直角、1px 边框、无阴影
- 圆角：仅保留必要的小圆点/仪表元素（本版本 CPU 仪表已改为直角分段条）

## 字体

- 数据正文：Sans 11
- 分组标题：Sans Bold，13–14
- 页面标题：Sans Bold，20–24
- 图表刻度：Sans 9–10

## 色彩

颜色只表达语义，统一由 support/palette.c 提供：

CPU 蓝、内存绿、磁盘读橙、磁盘写紫、
网络接收青、网络发送粉、温度红、GPU 黄。
