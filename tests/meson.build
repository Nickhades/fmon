# 单元测试：不依赖 GTK，只用 GLib 的测试框架
glib_dep = dependency('glib-2.0')
gio_dep  = dependency('gio-2.0')

# 折线图几何/量程/缺口/抽取/离屏渲染（纯 C + Cairo，不需要 GTK/X11）
test_chart = executable('test_chart', 'test_chart.c',
  files('../src/gui/charts/chart_geom.c',
        '../src/gui/charts/gui_chart_util.c',
        '../src/gui/charts/chart_geom_polyline.c',
        '../src/gui/charts/chart_geom_cairo.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/charts')],
  dependencies : [glib_dep, cairo_dep, m_dep],
  install : false
)
test('fmon_chart', test_chart, timeout : 30)

test_fidelity = executable('test_fidelity', 'test_fidelity.c',
  files('../src/gui/charts/chart_geom.c',
        '../src/gui/charts/chart_geom_polyline.c',
        '../src/gui/charts/chart_geom_cairo.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/charts')],
  dependencies : [glib_dep, cairo_dep, m_dep],
  install : false
)
test('fmon_fidelity', test_fidelity, timeout : 30)

test_history_provider = executable('test_history_provider', 'test_history_provider.c',
  files('../src/gui/history/history_provider.c',
        '../src/gui/history/history_parse.c'),
  include_directories : [common_inc, include_directories('../src/gui')],
  dependencies : [glib_dep, m_dep],
  install : false
)
test('fmon_history_provider', test_history_provider, timeout : 30)

test_child = executable('test_child', 'test_child.c',
  common_srcs,
  include_directories : common_inc,
  dependencies : [glib_dep, thread_dep],
  install : false
)
test('fmon_child', test_child, timeout : 30)

test_shm = executable('test_shm', 'test_shm.c',
  common_srcs,
  include_directories : common_inc,
  dependencies : [glib_dep, thread_dep],
  install : false
)
test('fmon_shm', test_shm, timeout : 60)

# ps 解析器是纯 GLib 代码（src/gui/views/process_ps.c），可直接单测
test_process_ps = executable('test_process_ps', 'test_process_ps.c',
  files('../src/gui/views/process_ps.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/views')],
  dependencies : [glib_dep,
                  meson.get_compiler('c').find_library('procstat',
                                                       required : true)],
  install : false
)
test('fmon_process_ps', test_process_ps, timeout : 30)

# rc.conf 解析器同样是纯 GLib 代码（src/gui/views/service_rc.c）
# CPU 健康判定是纯 C 函数，不依赖 GTK
test_cpu_health = executable('test_cpu_health', 'test_cpu_health.c',
  files('../src/gui/pages/cpu_health.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep],
  install : false
)
test('fmon_cpu_health', test_cpu_health, timeout : 30)

test_service_rc = executable('test_service_rc', 'test_service_rc.c',
  files('../src/gui/views/service_rc.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/views')],
  dependencies : [glib_dep],
  install : false
)
test('fmon_service_rc', test_service_rc, timeout : 30)

# 内存页说明文案（纯 GLib；文案里用了 _()，需要 GETTEXT_PACKAGE）
test_mem_summary = executable('test_mem_summary', 'test_mem_summary.c',
  files('../src/gui/pages/mem_summary.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep],
  c_args : ['-DGETTEXT_PACKAGE="fmon"'],
  install : false
)
test('fmon_mem_summary', test_mem_summary, timeout : 30)

# 内存队列占比（纯函数；读取依赖 sysctl，测试不触碰内核）
test_vm_stats = executable('test_vm_stats', 'test_vm_stats.c',
  files('../src/gui/pages/vm_stats.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep],
  install : false
)
test('fmon_vm_stats', test_vm_stats, timeout : 30)

# ZFS ARC 细节的纯函数部分（命中率/速率）
test_zfs_stats = executable('test_zfs_stats', 'test_zfs_stats.c',
  files('../src/gui/pages/zfs_stats.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep],
  install : false
)
test('fmon_zfs_stats', test_zfs_stats, timeout : 30)

# CPU 时间占比/速率纯函数
test_cpu_stats = executable('test_cpu_stats', 'test_cpu_stats.c',
  files('../src/gui/pages/cpu_stats.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep, m_dep],
  install : false
)
test('fmon_cpu_stats', test_cpu_stats, timeout : 30)

# vmstat -z 解析归类
test_kmem_stats = executable('test_kmem_stats', 'test_kmem_stats.c',
  files('../src/gui/pages/kmem_stats.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep, gio_dep],
  install : false
)
test('fmon_kmem_stats', test_kmem_stats, timeout : 30)

# 网络统计百分比
test_net_stats = executable('test_net_stats', 'test_net_stats.c',
  files('../src/gui/pages/net_stats.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep, gio_dep],
  install : false
)
test('fmon_net_stats', test_net_stats, timeout : 30)

# NVMe SMART 文本解析
devstat_lib = meson.get_compiler('c').find_library('devstat', required : true)
test_disk_perf = executable('test_disk_perf', 'test_disk_perf.c',
  files('../src/common/fmon_smart.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep, gio_dep, devstat_lib],
  install : false
)
test('fmon_disk_perf', test_disk_perf, timeout : 30)

# zpool status / list 解析
test_disk_queue = executable('test_disk_queue', 'test_disk_queue.c',
  files('../src/gui/pages/disk_queue.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep, gio_dep],
  install : false
)
test('fmon_disk_queue', test_disk_queue, timeout : 30)

test_zpool_health = executable('test_zpool_health', 'test_zpool_health.c',
  files('../src/gui/pages/zpool_health.c',
        '../src/gui/pages/zpool_parse.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/pages')],
  dependencies : [glib_dep, gio_dep],
  install : false
)
test('fmon_zpool_health', test_zpool_health, timeout : 30)

no_bezier = find_program('check_no_bezier.sh')
test('fmon_no_bezier', no_bezier, args : [meson.project_source_root()])

translations = find_program('check_translations.py')
test('fmon_translations', translations,
     args : [meson.project_source_root()], timeout : 60)

# 日志过滤纯函数（无 GTK）
test_log_filter = executable('test_log_filter', 'test_log_filter.c',
  files('../src/gui/views/log_filter.c'),
  include_directories : [common_inc,
                         include_directories('../src/gui/views')],
  dependencies : [glib_dep],
  install : false
)
test('fmon_log_filter', test_log_filter, timeout : 30)
