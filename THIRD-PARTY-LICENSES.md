# 第三方组件许可证

本程序（fmon）自身使用 **BSD 2-Clause** 许可证（见 `LICENSE`，版权人 Zhang Xin Yu）。
下面是它链接或调用的第三方组件及其许可证。本程序只动态链接这些库，不重新分发其源码。

## 界面

| 组件 | 许可证 | 版权 |
|---|---|---|
| GTK 4 | LGPL-2.1-or-later | Copyright (C) The GTK Team |
| GLib | LGPL-2.1-or-later | Copyright (C) GNOME Foundation |
| Pango | LGPL-2.1-or-later | Copyright (C) Owen Taylor, Red Hat, Inc. |
| cairo | LGPL-2.1 或 MPL-1.1（二选一） | Copyright (C) Carl Worth and contributors |
| GDK-Pixbuf | LGPL-2.1-or-later | Copyright (C) The GTK Team |

## 采集

| 组件 | 许可证 | 版权 |
|---|---|---|
| libkvm / libdevstat（FreeBSD 基础系统） | BSD-2-Clause | Copyright (C) The FreeBSD Project |
| libdrm / libdrm_amdgpu（AMD 传感器，可选） | MIT | Copyright (C) the libdrm authors |

## 运行时调用的外部命令

`ps`、`sockstat`、`zpool`、`sysrc`、`service` 等来自 FreeBSD 基础系统（BSD 许可证）；
`nvidia-smi` 来自 NVIDIA 驱动（专有软件）。这些程序由系统提供，本程序只调用、不重新分发。

## 许可证全文位置

- 各软件包：`/usr/local/share/licenses/<包名>/COPYING`
- FreeBSD 基础系统：`/usr/share/` 及源码树
- 本程序：`/usr/local/share/licenses/fmon/LICENSE`

LGPL 组件以动态链接方式使用；如需替换这些库，可直接替换系统中的同名共享库，
无需重新编译本程序。
