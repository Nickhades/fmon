#!/bin/sh
# 数据可视化路径必须只用直线/正交阶跃；禁止 Bézier/样条平滑。
set -eu
root="$1"
if grep -RInE --include='*.c' --include='*.h' \
    'cairo_curve_to|curve_to\(|bezier|Bezier|spline|Spline|catmull|Catmull|smoothstep' \
    "$root/src/gui" "$root/src/common"; then
    echo "forbidden curve/smoothing primitive found" >&2
    exit 1
fi
exit 0
