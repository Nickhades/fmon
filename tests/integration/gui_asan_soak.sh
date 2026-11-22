#!/bin/sh
# ASAN GUI smoke/soak. Usage: ./gui_asan_soak.sh [seconds] [asan-gui-binary]
set -eu

seconds="${1:-3600}"
binary="${2:-./build-asan-review/src/gui/fmon-gui}"
display="${DISPLAY:-:0.0}"
export DISPLAY="$display"

if [ ! -x "$binary" ]; then
    echo "missing ASAN GUI: $binary" >&2
    exit 2
fi

pkill -x fmon-gui 2>/dev/null || true
rm -f /tmp/fmon-asan-soak.log
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:allocator_may_return_null=1 \
    "$binary" --page cpu --theme dark >/tmp/fmon-asan-soak.log 2>&1 &
pid=$!
sleep 5
id=$(xwininfo -root -tree | grep -E 'FreeBSD 监控中心|FreeBSD Monitoring Center' | tail -1 | awk '{print $1}')
if [ -z "$id" ]; then
    echo "window not found" >&2
    kill "$pid" 2>/dev/null || true
    exit 1
fi
left=$(xwininfo -id "$id" | awk '/Absolute upper-left X/{print $4}')
top=$(xwininfo -id "$id" | awk '/Absolute upper-left Y/{print $4}')

elapsed=0
while [ "$elapsed" -lt "$seconds" ]; do
    case $((elapsed / 30 % 5)) in
        0) y=120 ;; 1) y=170 ;; 2) y=220 ;; 3) y=270 ;; 4) y=70 ;;
    esac
    if command -v /tmp/xclick >/dev/null 2>&1; then
        /tmp/xclick "$((left + 80))" "$((top + y))" 2>/dev/null || true
    fi
    sleep 30
    elapsed=$((elapsed + 30))
done

if [ -x /tmp/xclose ]; then
    /tmp/xclose "$id" 2>/dev/null || true
fi
sleep 2
kill -TERM "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
if grep -q 'AddressSanitizer' /tmp/fmon-asan-soak.log; then
    echo "ASAN error detected" >&2
    tail -40 /tmp/fmon-asan-soak.log >&2
    exit 1
fi
echo "ASAN soak passed: ${seconds}s"
