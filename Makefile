# BSD make 快捷构建（正式构建请用 meson，见 README.md）
CC      ?= cc
# -Wno-unused-parameter 必须放在 -Wextra 之后，否则会被重新打开
CFLAGS  ?= -O2 -std=c11 -pthread \
           -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wpointer-arith -Wwrite-strings -Wvla \
           -fstack-protector-strong -Wno-unused-parameter
LDFLAGS ?= -pthread -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

# bmake 特有的 != 语法：从 pkg-config 取 GTK4 参数
GTK_CFLAGS != pkg-config --cflags gtk4 cairo
GTK_LIBS   != pkg-config --libs gtk4 cairo

# FreeBSD 系统库
SYS_LIBS = -lkvm -ldevstat -lm

# 可选：libdrm_amdgpu（AMD 显卡负载）。装了 libdrm 就自动启用。
.if exists(/usr/local/include/libdrm/amdgpu.h)
CFLAGS   += -DFMON_HAVE_AMDGPU=1 -I/usr/local/include
SYS_LIBS += -L/usr/local/lib -ldrm_amdgpu
.endif

COMMON_SRC = src/common/fmon_shm.c src/common/fmon_child.c

DAEMON_SRC = src/daemon/fmon_daemon.c \
             src/daemon/collector_cpu.c \
             src/daemon/collector_io.c \
             src/daemon/collector_net.c \
             src/daemon/collector_storage.c \
             src/daemon/collector_proc.c \
             src/daemon/collector_zfs.c

GUI_SRC = src/gui/main.c \
          src/gui/gui_util.c \
          src/gui/gui_chart.c \
          src/gui/process_view.c \
          src/gui/service_view.c \
          src/gui/storage_view.c \
          src/gui/network_view.c \
          src/gui/log_view.c

all: fmon-daemon fmon-gui

fmon-daemon: $(DAEMON_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) -Isrc/common -Isrc/daemon -o $@ \
	    $(DAEMON_SRC) $(COMMON_SRC) $(LDFLAGS) $(SYS_LIBS)

fmon-gui: $(GUI_SRC) $(COMMON_SRC)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Isrc/common -Isrc/gui -o $@ \
	    $(GUI_SRC) $(COMMON_SRC) $(LDFLAGS) $(GTK_LIBS) $(SYS_LIBS)

clean:
	rm -f fmon-daemon fmon-gui

.PHONY: all clean
