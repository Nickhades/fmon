gnome = import('gnome')

gui_srcs = files(
  'app/main.c',
  'app/gui_window.c',
  'app/gui_export.c',
  'app/gui_runtime.c',
  'app/gui_snapshot.c',
  'app/gui_theme_ui.c',
  'app/gui_window_events.c',
  'app/gui_nav.c',
  'app/gui_nav_pages.c',
  'app/gui_css.c',
  'app/gui_css_win11b.c',
  'app/gui_css_win11.c',
  'app/gui_css_tables.c',
  'app/gui_css_main.c',
  'charts/chart_geom.c',
  'charts/gui_chart.c',
  'charts/gui_chart_util.c',
  'charts/combo_chart.c',
  'charts/combo_chart_draw.c',
  'charts/combo_chart_geom.c',
  'pages/gui_coreview.c',
  'pages/gui_page_state.c',
  'pages/gui_page_build.c',
  'pages/gui_pages_layout.c',
  'pages/gui_refresh.c',
  'pages/mem_summary.c',
  'pages/vm_stats.c',
  'pages/zfs_stats.c',
  'pages/cpu_stats.c',
  'pages/kmem_stats.c',
  'pages/net_stats.c',
  'pages/disk_perf.c',
  'pages/disk_queue.c',
  'pages/collector_thread.c',
  'pages/zpool_health.c',
  'pages/gui_page_cpu.c',
  'pages/cpu_health.c',
  'pages/gui_page_l2arc.c',
  'pages/gui_pages.c',
  'pages/gui_page_gpu.c',
  'pages/gui_page_net.c',
  'pages/gui_page_zfs.c',
  'pages/gui_page_disk.c',
  'pages/gui_page_mem.c',
  'pages/gui_page_batt.c',
  'support/gui_widgets.c',
  'support/gui_util.c',
  'support/about.c',
  'support/font_setup.c',
  'support/theme.c',
  'support/alerts.c',
  'support/cpuset_dialog.c',
  'support/gui_ctl.c',
  'support/disk_history.c',
  'views/alert_view.c',
  'views/rctl_view.c',
  'views/history_view.c',
  'views/process_ps.c',
  'views/process_model.c',
  'views/process_actions.c',
  'views/process_list.c',
  'views/process_windows.c',
  'views/process_view.c',
  'views/service_rc.c',
  'views/service_model.c',
  'views/service_actions.c',
  'views/service_list.c',
  'views/service_view.c',
  'views/network_view.c',
  'views/storage_view.c',
  'views/log_view.c'
)

# 自带 SVG 图标：编进二进制，不依赖桌面图标主题（KDE/Breeze 下常缺
# `*-symbolic` 名称，会导致导航栏显示“缺图”占位符）。
# 资源路径为 /org/fmon/icons/scalable/actions/fmon-*-symbolic.svg。
fmon_icons = gnome.compile_resources(
  'fmon-icons',
  'assets/fmon-icons.gresource.xml',
  source_dir : 'assets',
  c_name : 'fmon_icons',
)

gui_srcs = gui_srcs + fmon_icons

# 应用图标与桌面入口：图标为项目自绘 SVG，不依赖系统图标主题。
install_data(
  'assets/scalable/apps/org.freebsd.fmon.svg',
  install_dir : join_paths(get_option('datadir'), 'icons', 'hicolor',
                           'scalable', 'apps')
)
install_data(
  'assets/org.freebsd.fmon.desktop',
  install_dir : join_paths(get_option('datadir'), 'applications')
)

# 语言包目录随安装前缀；localedir 若是绝对路径则 join_paths 会直接采用
locale_dir = join_paths(get_option('prefix'), get_option('localedir'))

executable('fmon-gui',
  gui_srcs + common_srcs,
  c_args : ['-DGETTEXT_PACKAGE="fmon"',
            '-DLOCALEDIR="' + locale_dir + '"'],
  include_directories : [common_inc, include_directories('.', 'app', 'charts', 'pages', 'views', 'support')],
  dependencies : [thread_dep, gtk_dep, cairo_dep, m_dep, fontconfig_dep,
                  dependency('sqlite3'),
                  meson.get_compiler('c').find_library('procstat', required : true),
                  meson.get_compiler('c').find_library('devstat', required : true),
                  meson.get_compiler('c').find_library('execinfo', required : true)],
  install : true,
  install_dir : get_option('bindir')
)
