#!/bin/sh
#
# fmon 一键安装 / 卸载脚本（FreeBSD 15+）
#
#   安装：sudo ./install.sh
#   卸载：sudo ./install.sh --uninstall
#   只看会做什么：./install.sh --dry-run -y
#   只编译不安装：./install.sh --build-only
#
# 设计原则：
#   1. 幂等：重复执行不会产生副作用（重复安装会先停旧服务再装新版本）。
#   2. 不猜测：依赖缺失时明确告诉你装哪个包；编译 0 warning 才算成功。
#   3. 可回退：卸载会停服务、取消开机自启、删除二进制与 rc 脚本，
#      并清理共享内存与锁文件（可用 --keep-data 保留）。
#   4. 可预演：--dry-run 打印每一步，不修改系统。

set -eu

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------
VERSION="1.1.1"

SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/$(basename -- "$0")
SRC_DIR=$(dirname -- "$SELF")
BUILD_DIR="$SRC_DIR/build"

# 安装前缀：默认 FreeBSD 惯例 /usr/local；可用 --prefix 或环境变量覆盖。
# 所有安装路径都从这里派生，避免写死。
PREFIX="${PREFIX:-/usr/local}"

BIN_DAEMON="$PREFIX/sbin/fmon-daemon"
BIN_GUI="$PREFIX/bin/fmon-gui"
RC_SCRIPT="$PREFIX/etc/rc.d/fmon_daemon"
LEGACY_BIN="$PREFIX/bin/fmon_daemon"
LICENSE_DIR="$PREFIX/share/licenses/fmon"
LOCK_FILE="/var/run/fmon-daemon.lock"

LOG_FILE="/var/log/fmon-install.log"

RC_SERVICE="fmon_daemon"
RC_VAR="${RC_SERVICE}_enable"

PKG_DEPS="meson ninja pkgconf gtk4 cairo dmidecode"
PKG_OPTIONAL="libdrm"

# ---------------------------------------------------------------------------
# 参数
# ---------------------------------------------------------------------------
ASSUME_YES=0
SKIP_DEPS=0
DO_CLEAN=0
NO_START=0
BUILD_ONLY=0
DRY_RUN=0
UNINSTALL=0
LAUNCH_GUI=0
KEEP_LEGACY=0
KEEP_DATA=0

# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    C_OK='\033[32m'; C_WARN='\033[33m'; C_ERR='\033[31m'
    C_INFO='\033[36m'; C_BOLD='\033[1m'; C_OFF='\033[0m'
else
    C_OK=''; C_WARN=''; C_ERR=''; C_INFO=''; C_BOLD=''; C_OFF=''
fi

log_raw() {
    [ -w "$LOG_FILE" ] || return 0
    printf '%s\n' "$*" >>"$LOG_FILE"
}

info() {
    printf '%s[i]%s %s\n' "$C_INFO" "$C_OFF" "$*"
    log_raw "[i] $*"
}

ok() {
    printf '%s[✓]%s %s\n' "$C_OK" "$C_OFF" "$*"
    log_raw "[ok] $*"
}

warn() {
    printf '%s[!]%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2
    log_raw "[warn] $*"
}

die() {
    printf '%s[✗]%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2
    log_raw "[error] $*"
    printf '\n安装中止。日志：%s\n' "$LOG_FILE" >&2
    exit 1
}

step() {
    printf '\n%s==> %s%s\n' "$C_BOLD" "$*" "$C_OFF"
    log_raw ""
    log_raw "==> $*"
}

usage() {
    cat <<EOF
fmon ${VERSION} 一键安装脚本

用法：
  sudo ./install.sh [选项]

选项：
  -y, --yes          不询问，直接执行（自动化/CI 用）
  --dry-run          只打印将要执行的步骤，不修改系统
  --build-only       只编译，不安装（普通用户可用）
  --clean            安装前删除 build/ 目录重新配置
  --skip-deps        跳过依赖安装（缺依赖时直接报错）
  --no-start         安装后不启动服务
  --launch           安装后尝试以当前桌面用户启动图形界面
  --uninstall        卸载（停服务 + 取消自启 + 删除文件）
  --keep-legacy      保留旧版 /usr/local/bin/fmon_daemon 不备份
  --keep-data        卸载时保留共享内存与锁文件
  -h, --help         显示本帮助

示例：
  sudo ./install.sh                    # 标准安装
  sudo ./install.sh --clean -y         # 全量重编译并安装
  ./install.sh --build-only -y         # 只编译（不碰系统目录）
  ./install.sh --dry-run -y            # 预演安装流程
  sudo ./install.sh --uninstall        # 卸载

说明：
  * 安装内容：${BIN_DAEMON}、${BIN_GUI}、${RC_SCRIPT}
  * 开机自启：sysrc ${RC_VAR}=YES
  * 日志：${LOG_FILE}
EOF
}

# ---------------------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------------------
run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        info "[dry-run] $*"
        return 0
    fi
    log_raw "\$ $*"
    "$@"
}

have() {
    command -v "$1" >/dev/null 2>&1
}

# 系统 pkg 的绝对路径：PATH 里可能被同名工具（例如 node 的 pkg）抢占，
# 这里优先使用 FreeBSD 自带的 /usr/sbin/pkg。
# 共享内存对象名随 ABI 版本变化，这里从二进制自身读取，避免版本升级后写死失效
detect_shm_name() {
    ver=""
    if [ -x "$BIN_DAEMON" ]; then
        ver=$("$BIN_DAEMON" --version 2>/dev/null | sed -n 's/.*ABI \([0-9][0-9]*\).*/\1/p')
    fi
    if [ -z "$ver" ] && [ -x "$BUILD_DIR/src/daemon/fmon-daemon" ]; then
        ver=$("$BUILD_DIR/src/daemon/fmon-daemon" --version 2>/dev/null | \
              sed -n 's/.*ABI \([0-9][0-9]*\).*/\1/p')
    fi
    [ -n "$ver" ] || return 1
    printf '/fmon_daemon_shm_v%s' "$ver"
}

find_pkg() {
    for candidate in /usr/sbin/pkg /usr/local/sbin/pkg; do
        if [ -x "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    command -v pkg 2>/dev/null || true
}

confirm() {
    # $1 = 问题；返回 0 表示“是”
    if [ "$ASSUME_YES" -eq 1 ]; then
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        info "[dry-run] 假设回答“是”：$1"
        return 0
    fi
    if [ ! -t 0 ]; then
        die "当前不是交互终端，请加 -y 参数确认执行"
    fi
    printf '%s [y/N] ' "$1"
    read -r answer
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# 参数解析
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)        ASSUME_YES=1 ;;
        --dry-run)       DRY_RUN=1; ASSUME_YES=1 ;;
        --build-only)    BUILD_ONLY=1 ;;
        --clean)         DO_CLEAN=1 ;;
        --skip-deps)     SKIP_DEPS=1 ;;
        --no-start)      NO_START=1 ;;
        --launch)        LAUNCH_GUI=1 ;;
        --uninstall)     UNINSTALL=1 ;;
        --keep-legacy)   KEEP_LEGACY=1 ;;
        --keep-data)     KEEP_DATA=1 ;;
        --prefix=*)      PREFIX="${1#--prefix=}" ;;
        --prefix)        shift; PREFIX="${1:?--prefix 需要参数}" ;;
        -h|--help)       usage; exit 0 ;;
        *)               printf '未知参数：%s\n\n' "$1" >&2; usage; exit 2 ;;
    esac
    shift
done

PKG_CMD=$(find_pkg)

# 参数可能修改了 --prefix：统一按最终前缀重算所有安装路径
BIN_DAEMON="$PREFIX/sbin/fmon-daemon"
BIN_GUI="$PREFIX/bin/fmon-gui"
RC_SCRIPT="$PREFIX/etc/rc.d/fmon_daemon"
LEGACY_BIN="$PREFIX/bin/fmon_daemon"
LICENSE_DIR="$PREFIX/share/licenses/fmon"

# ---------------------------------------------------------------------------
# 权限
# ---------------------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    if [ "$BUILD_ONLY" -eq 1 ]; then
        :   # 只编译，不需要 root
    elif [ "$DRY_RUN" -eq 1 ]; then
        warn "当前不是 root：--dry-run 只打印步骤，不会改动系统"
    else
        if have sudo; then
            info "安装需要 root 权限，正在通过 sudo 重新执行…"
            exec sudo -- "$SELF" "$@"
        else
            die "请以 root 运行：su -c '$SELF'，或先安装 sudo"
        fi
    fi
fi

# 日志（普通用户执行 --build-only 时回退到 /tmp）
if ! { [ -e "$LOG_FILE" ] && [ -w "$LOG_FILE" ]; } 2>/dev/null; then
    if [ "$(id -u)" -eq 0 ]; then
        : >"$LOG_FILE" 2>/dev/null || LOG_FILE="/tmp/fmon-install.log"
    else
        LOG_FILE="/tmp/fmon-install.log"
    fi
    : >"$LOG_FILE" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 环境检查
# ---------------------------------------------------------------------------
check_os() {
    step "环境检查"

    if [ "$(uname -s)" != "FreeBSD" ]; then
        die "本项目依赖 FreeBSD 特有接口（kvm / devstat / sysctl），当前系统：$(uname -s)"
    fi
    ok "系统：FreeBSD $(uname -r)"

    if [ ! -f "$SRC_DIR/meson.build" ] || [ ! -d "$SRC_DIR/src" ]; then
        die "当前目录不像 fmon 源码目录：$SRC_DIR（缺少 meson.build 或 src/）"
    fi
    ok "源码目录：$SRC_DIR"

    ARCH=$(uname -m)
    ok "架构：$ARCH"

    if [ "$(id -u)" -eq 0 ]; then
        if [ -n "${SUDO_USER:-}" ]; then
            ok "以 sudo 方式运行（原用户：${SUDO_USER}）"
        else
            warn "以 root 身份直接运行：build/ 目录将属于 root"
        fi
    fi
}

# ---------------------------------------------------------------------------
# 依赖
# ---------------------------------------------------------------------------
check_deps() {
    step "依赖检查"

    [ -n "$PKG_CMD" ] || die "找不到系统 pkg（/usr/sbin/pkg）。FreeBSD 上应默认存在，请检查系统完整性"

    # 用 pkg-config 判断库是否可用，而不是 pkg 数据库：
    # 这样用 ports 安装的 GTK/Cairo 也会被正确识别，不会触发 pkg 覆盖安装。
    missing_pkgs=""

    for tool in meson ninja pkgconf; do
        have "$tool" || missing_pkgs="$missing_pkgs $tool"
    done

    if have pkg-config; then
        pkg-config --exists gtk4  || missing_pkgs="$missing_pkgs gtk4"
        pkg-config --exists cairo || missing_pkgs="$missing_pkgs cairo"
    else
        # 连 pkg-config 都没有：直接整包装
        missing_pkgs="$missing_pkgs gtk4 cairo"
    fi

    if [ -n "$missing_pkgs" ]; then
        info "缺少依赖：$missing_pkgs"

        if [ "$SKIP_DEPS" -eq 1 ]; then
            die "缺少依赖且指定了 --skip-deps。请先执行：
    $PKG_CMD install -y $missing_pkgs"
        fi

        info "正在安装：$missing_pkgs"
        # shellcheck disable=SC2086
        run "$PKG_CMD" install -y $missing_pkgs || \
            die "依赖安装失败，请检查 /etc/pkg/FreeBSD.conf 与网络"

        if [ "$DRY_RUN" -eq 0 ]; then
            have pkg-config && pkg-config --exists gtk4 && pkg-config --exists cairo || \
                warn "pkg install 完成，但 pkg-config 仍找不到 gtk4/cairo，请检查环境变量 PKG_CONFIG_PATH"
        fi
        ok "依赖已安装"
    else
        ok "必需依赖齐全（meson / ninja / pkgconf / gtk4 / cairo）"
    fi

    # 内存条信息（需要 dmidecode，读取 SMBIOS）
    if have dmidecode; then
        ok "内存条信息：dmidecode 可用（可显示插槽/容量/频率）"
    else
        warn "未找到 dmidecode：内存条详情不可用（$PKG_CMD install -y dmidecode）"
    fi

    # 可选依赖：AMD 显卡负载
    if have pkg-config && pkg-config --exists libdrm_amdgpu; then
        ok "可选依赖 libdrm_amdgpu：已安装（AMD 显卡可读真实负载/温度）"
    else
        warn "可选依赖 libdrm_amdgpu：未安装 → AMD 显卡只能显示设备信息，没有负载数据
      需要的话执行：$PKG_CMD install -y libdrm"
    fi

    # 可选依赖：NVIDIA 遥测（PATH 优先，其次常见目录；不写死单一位置）
    NV_SMI=""
    if have nvidia-smi; then
        NV_SMI="$(command -v nvidia-smi)"
    else
        for p in "$PREFIX/bin/nvidia-smi" /usr/local/bin/nvidia-smi \
                 /usr/bin/nvidia-smi /sbin/nvidia-smi \
                 /usr/local/nvidia/bin/nvidia-smi; do
            [ -x "$p" ] && NV_SMI="$p" && break
        done
    fi
    if [ -n "$NV_SMI" ]; then
        ok "NVIDIA 遥测：$NV_SMI 可用（独显负载/显存/温度可读）"
    else
        warn "未检测到 nvidia-smi：可安装驱动（$PKG_CMD install -y nvidia-driver），
      或若使用自编译驱动，启动服务时设置 FMON_NVIDIA_SMI=/你的路径/nvidia-smi"
    fi

    # 可选：nvtop（仅作为对照工具，本程序不依赖它）
    NVTOP=""
    if have nvtop; then
        NVTOP="$(command -v nvtop)"
    else
        for p in /usr/local/bin/nvtop /usr/bin/nvtop; do
            [ -x "$p" ] && NVTOP="$p" && break
        done
    fi
    if [ -n "$NVTOP" ]; then
        ok "对照工具 nvtop：$NVTOP"
    else
        info "可选工具 nvtop 未安装；需要对照查看多 GPU 时可执行：$PKG_CMD install -y nvtop"
    fi

    for tool in meson ninja cc; do
        have "$tool" || die "找不到 $tool，请确认 PATH 或重新安装依赖"
    done
    ok "构建工具：meson $(meson --version)、ninja $(ninja --version)、$(cc --version | head -n1)"
}

# ---------------------------------------------------------------------------
# 构建
# ---------------------------------------------------------------------------
do_build() {
    step "编译"

    cd "$SRC_DIR"

    if [ "$DO_CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
        info "删除旧构建目录 $BUILD_DIR"
        run rm -rf "$BUILD_DIR"
    fi

    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        info "配置构建目录（prefix=$PREFIX）…"
        run meson setup "$BUILD_DIR" --buildtype=release --prefix="$PREFIX" \
            || die "meson setup 失败"
    else
        info "复用已有构建目录（改了 meson.build 会自动重新配置）"
        run meson configure "$BUILD_DIR" --prefix="$PREFIX" >/dev/null 2>&1 || true
    fi

    info "开始编译…"
    if [ "$DRY_RUN" -eq 1 ]; then
        info "[dry-run] ninja -C $BUILD_DIR"
        return 0
    fi
    log_raw "\$ ninja -C $BUILD_DIR"
    ninja -C "$BUILD_DIR" || die "编译失败，请查看上面的错误信息"
    ok "编译完成"

    for f in "$BUILD_DIR/src/daemon/fmon-daemon" "$BUILD_DIR/src/gui/fmon-gui"; do
        [ -x "$f" ] || die "预期产物不存在：$f"
    done
    ok "产物：$BUILD_DIR/src/{daemon/fmon-daemon, gui/fmon-gui}"

    # 编译期特性提示
    if have pkg-config && pkg-config --exists libdrm_amdgpu; then
        ok "AMD GPU 传感器：已编译进守护进程"
    fi
}

# ---------------------------------------------------------------------------
# 停止旧进程
# ---------------------------------------------------------------------------
stop_running() {
    step "停止正在运行的旧实例"

    if [ -x "$RC_SCRIPT" ] && service "$RC_SERVICE" status >/dev/null 2>&1; then
        info "停止 rc 服务 $RC_SERVICE…"
        run service "$RC_SERVICE" stop || warn "service stop 返回非 0，继续"
    fi

    # 兼容旧版（/usr/local/bin/fmon_daemon）与手动启动的实例
    if pgrep -f '/fmon[-_]daemon' >/dev/null 2>&1; then
        info "结束残留的 fmon 守护进程…"
        run pkill -TERM -f '/fmon[-_]daemon' 2>/dev/null || true

        if [ "$DRY_RUN" -eq 0 ]; then
            i=0
            while pgrep -f '/fmon[-_]daemon' >/dev/null 2>&1 && [ "$i" -lt 10 ]; do
                sleep 1
                i=$((i + 1))
            done
            if pgrep -f '/fmon[-_]daemon' >/dev/null 2>&1; then
                warn "仍有进程未退出，强制结束"
                run pkill -KILL -f '/fmon[-_]daemon' 2>/dev/null || true
                sleep 1
            fi
        fi
    fi

    ok "没有遗留的 fmon 进程"
}

# ---------------------------------------------------------------------------
# 安装
# ---------------------------------------------------------------------------
install_locales() {
    local mo lang

    for mo in "$BUILD_DIR"/po/*.mo; do
        [ -f "$mo" ] || continue
        lang=$(basename "$mo" .mo)
        run install -d -m 0755 "$LOCALEDIR/$lang/LC_MESSAGES"
        run install -m 0644 "$mo" "$LOCALEDIR/$lang/LC_MESSAGES/fmon.mo"
    done
}

install_files() {
    step "安装文件"

    cd "$SRC_DIR"
    run ninja -C "$BUILD_DIR" install || die "ninja install 失败"

    [ "$DRY_RUN" -eq 1 ] || [ -x "$BIN_DAEMON" ] || die "安装后找不到 $BIN_DAEMON"
    [ "$DRY_RUN" -eq 1 ] || [ -x "$BIN_GUI" ]    || die "安装后找不到 $BIN_GUI"
    [ "$DRY_RUN" -eq 1 ] || [ -x "$RC_SCRIPT" ]  || die "安装后找不到 $RC_SCRIPT"

    ok "已安装：$BIN_DAEMON"
    ok "已安装：$BIN_GUI"
    ok "已安装：$RC_SCRIPT"

    # 旧版（下划线命名）的二进制：默认备份后移除，避免两个守护进程打架
    if [ -f "$LEGACY_BIN" ] && [ "$KEEP_LEGACY" -eq 0 ]; then
        if confirm "发现旧版二进制 $LEGACY_BIN，备份为 ${LEGACY_BIN}.bak 并移除？"; then
            run mv -f "$LEGACY_BIN" "${LEGACY_BIN}.bak"
            ok "旧版二进制已备份为 ${LEGACY_BIN}.bak"
        else
            warn "保留旧版二进制；若它仍被 rc 启动，会与新版同时运行并各自使用不同的共享内存"
        fi
    fi
}

# ---------------------------------------------------------------------------
# 开机自启
# ---------------------------------------------------------------------------
enable_service() {
    step "配置开机自启"

    if [ "$(sysrc -n "$RC_VAR" 2>/dev/null || true)" = "YES" ]; then
        ok "$RC_VAR 已经是 YES"
    else
        run sysrc "${RC_VAR}=YES" || die "sysrc 设置失败"
        ok "已设置 ${RC_VAR}=YES"
    fi

    # rc.conf 里如果还留着旧版（下划线）的开关，提示一下
    if [ "$(sysrc -n fmon_daemon_enable 2>/dev/null || true)" = "YES" ] && \
       [ "$RC_VAR" != "fmon_daemon_enable" ]; then
        warn "检测到旧版开关 fmon_daemon_enable=YES，可能需要手动关闭"
    fi
}

# ---------------------------------------------------------------------------
# 启动 + 健康检查
# ---------------------------------------------------------------------------
start_service() {
    step "启动服务"

    if [ "$NO_START" -eq 1 ]; then
        warn "指定了 --no-start，跳过启动"
        return 0
    fi

    run service "$RC_SERVICE" start || die "服务启动失败：请查看 $LOG_FILE 与 /var/log/messages"

    if [ "$DRY_RUN" -eq 1 ]; then
        return 0
    fi

    # 等待进程真正起来（最多 10 秒）
    i=0
    while [ "$i" -lt 10 ]; do
        if service "$RC_SERVICE" status >/dev/null 2>&1; then
            break
        fi
        sleep 1
        i=$((i + 1))
    done

    service "$RC_SERVICE" status >/dev/null 2>&1 || \
        die "服务未能启动。排查：
    service $RC_SERVICE status
    tail -n 50 /var/log/messages
    tail -n 50 /var/log/fmon-daemon.log 2>/dev/null"
    ok "服务已启动"
}

health_check() {
    step "健康检查"

    if [ "$DRY_RUN" -eq 1 ] || [ "$NO_START" -eq 1 ]; then
        info "跳过（服务未启动）"
        return 0
    fi

    info "版本：$("$BIN_DAEMON" --version 2>/dev/null || echo '无法获取')"

    # 共享内存是否创建成功（名字随 ABI 版本变化，动态探测）
    shm_name=$(detect_shm_name || true)
    if have posixshmcontrol && [ -n "$shm_name" ]; then
        if posixshmcontrol ls 2>/dev/null | grep -q "$shm_name"; then
            ok "共享内存已创建：$shm_name"
        else
            warn "没有找到共享内存 $shm_name（守护进程可能启动失败）"
        fi
    fi

    # GPU 探测结果
    printf '\n  已探测到的显卡：\n'
    found_gpu=0
    if have nvidia-smi; then
        nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | \
            while IFS= read -r line; do
                printf '    - NVIDIA: %s\n' "$line"
            done
        found_gpu=1
    fi
    i=0
    while [ "$i" -lt 8 ]; do
        pci=$(sysctl -n "dev.drm.${i}.PCI_ID" 2>/dev/null || true)
        if [ -n "$pci" ]; then
            printf '    - DRM 设备 %s（PCI %s）\n' "$i" "$pci"
            found_gpu=1
        fi
        i=$((i + 1))
    done
    if [ "$found_gpu" -eq 0 ]; then
        printf '    （未检测到显卡驱动，GPU 卡片会显示 N/A）\n'
    fi

    if have pkg-config && pkg-config --exists libdrm_amdgpu; then
        printf '    AMD 传感器：libdrm_amdgpu 已安装\n'
    else
        printf '    AMD 传感器：未安装 libdrm（只能显示设备信息）\n'
    fi
    printf '\n'
}

# ---------------------------------------------------------------------------
# 收尾
# ---------------------------------------------------------------------------
fix_ownership() {
    # 用 sudo 运行时，把 build/ 交还给原用户，避免以后普通用户构建失败
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ] && [ -d "$BUILD_DIR" ]; then
        if [ "$DRY_RUN" -eq 0 ]; then
            chown -R "$SUDO_USER" "$BUILD_DIR" 2>/dev/null || true
        fi
    fi
}

summary() {
    step "安装完成"

    printf '  %-12s %s\n' "版本"     "$VERSION"
    printf '  %-12s %s\n' "守护进程" "$BIN_DAEMON"
    printf '  %-12s %s\n' "图形界面" "$BIN_GUI"
    printf '  %-12s %s\n' "启动脚本" "$RC_SCRIPT"
    printf '  %-12s %s\n' "安装日志" "$LOG_FILE"
    printf '\n常用命令：\n'
    printf '  %s\n' "service $RC_SERVICE status|start|stop|restart"
    printf '  %s\n' "sysrc ${RC_VAR}=YES          # 开机自启"
    printf '  %s\n' "fmon-gui                     # 以普通用户打开界面"
    printf '  %s\n' "fmon-gui --page zfs          # 直接打开指定页面"
    printf '  %s\n' "fmon-gui --gpu 1             # 多显卡时默认显示第 2 张"
    printf '\n卸载：sudo %s --uninstall\n\n' "$0"
}

maybe_launch_gui() {
    [ "$LAUNCH_GUI" -eq 1 ] || return 0
    [ "$DRY_RUN" -eq 0 ] || { info "[dry-run] 启动图形界面"; return 0; }
    [ "$NO_START" -eq 0 ] || return 0

    if [ -z "${SUDO_USER:-}" ]; then
        warn "--launch 需要以 sudo 方式运行，才能用桌面用户身份打开界面"
        return 0
    fi

    if [ -z "${DISPLAY:-}" ]; then
        warn "--launch 需要 DISPLAY 环境变量（请在桌面终端里执行 sudo -E ./install.sh --launch）"
        return 0
    fi

    info "以 ${SUDO_USER} 身份启动图形界面…"
    su -m "$SUDO_USER" -c "$BIN_GUI >/dev/null 2>&1 &" 2>/dev/null || \
        warn "启动失败，请手动运行：$BIN_GUI"
}

# ---------------------------------------------------------------------------
# 卸载
# ---------------------------------------------------------------------------
do_uninstall() {
    step "卸载 fmon"

    printf '将执行：\n'
    printf '  - 停止并禁用服务 %s\n' "$RC_SERVICE"
    printf '  - 删除 %s\n' "$BIN_DAEMON"
    printf '  - 删除 %s\n' "$BIN_GUI"
    printf '  - 删除 %s\n' "$RC_SCRIPT"
    if [ "$KEEP_DATA" -eq 0 ]; then
        printf '  - 清理共享内存 %s 与锁文件 %s\n' \
            "$(detect_shm_name || echo '/fmon_daemon_shm_*')" "$LOCK_FILE"
    fi
    printf '\n'

    confirm "确认卸载？" || { info "已取消"; exit 0; }

    stop_running

    step "取消开机自启"
    run sysrc -x "$RC_VAR" 2>/dev/null || true
    ok "已移除 ${RC_VAR}"

    step "删除文件"
    for f in "$BIN_DAEMON" "$BIN_GUI" "$RC_SCRIPT"; do
        if [ -e "$f" ]; then
            run rm -f "$f"
            ok "已删除 $f"
        fi
    done

    if [ -f "${LEGACY_BIN}.bak" ]; then
        if confirm "恢复旧版二进制 ${LEGACY_BIN}.bak → ${LEGACY_BIN}？"; then
            run mv -f "${LEGACY_BIN}.bak" "$LEGACY_BIN"
            ok "已恢复旧版二进制"
        fi
    fi

    if [ "$KEEP_DATA" -eq 0 ]; then
        step "清理共享内存与锁文件"
        shm_name=$(detect_shm_name || true)
        if have posixshmcontrol && [ -n "$shm_name" ]; then
            run posixshmcontrol rm "$shm_name" 2>/dev/null || \
                warn "共享内存 $shm_name 不存在或已清理"
        fi
        run rm -f "$LOCK_FILE"
        ok "已清理"
    else
        info "保留共享内存与锁文件（--keep-data）"
    fi

    step "卸载完成"
    printf '  源码与 build/ 目录未删除，如需清理：rm -rf %s\n' "$BUILD_DIR"
    printf '  安装日志保留在 %s\n\n' "$LOG_FILE"
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
main() {
    if [ "$UNINSTALL" -eq 1 ]; then
        log_raw "==== fmon ${VERSION} uninstall $(date '+%Y-%m-%d %H:%M:%S') ===="
    else
        log_raw "==== fmon ${VERSION} install $(date '+%Y-%m-%d %H:%M:%S') ===="
    fi

    check_os

    if [ "$UNINSTALL" -eq 1 ]; then
        do_uninstall
        return 0
    fi

    check_deps
    do_build

    if [ "$BUILD_ONLY" -eq 1 ]; then
        step "完成（--build-only，未安装）"
        printf '  产物：%s/src/daemon/fmon-daemon\n' "$BUILD_DIR"
        printf '        %s/src/gui/fmon-gui\n\n' "$BUILD_DIR"
        fix_ownership
        return 0
    fi

    stop_running
    install_files
    install_locales
    enable_service
    start_service
    health_check
    fix_ownership
    summary
    maybe_launch_gui
}

main "$@"
