# BSD make 快捷入口（正式构建仍以 meson 为准，见 README.md）
#
# 旧版 Makefile 复制了一份手工维护的源文件列表，和 meson 早已不一致，
# 照它编译会缺符号。这里不再重复列表，只做 meson/ninja 的薄封装，
# 保证两种入口永远编译同一套源码。

BUILD_DIR ?= build
PREFIX    ?= /usr/local
MESON     ?= meson
NINJA     ?= ninja

all:
	@if [ ! -f "$(BUILD_DIR)/build.ninja" ]; then \
		$(MESON) setup "$(BUILD_DIR)" --buildtype=release --prefix="$(PREFIX)"; \
	fi
	$(NINJA) -C "$(BUILD_DIR)"

test: all
	$(MESON) test -C "$(BUILD_DIR)" --print-errorlogs

install: all
	$(NINJA) -C "$(BUILD_DIR)" install

clean:
	rm -rf "$(BUILD_DIR)"

.PHONY: all test install clean
