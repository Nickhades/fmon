daemon_srcs = files(
  'core/fmon_daemon.c',
  'core/fmon_ctl.c',
  'core/history_store.c',
  'collectors/collector_cpu.c',
  'collectors/collector_io.c',
  'collectors/collector_gpu.c',
  'collectors/collector_net.c',
  'collectors/collector_storage.c',
  'collectors/collector_proc.c',
  'collectors/collector_zfs.c',
  'collectors/collector_meminfo.c'
)

daemon_deps  = [thread_dep, kvm_dep, devstat_dep, m_dep]
daemon_cargs = []

# 可选依赖：有 libdrm_amdgpu 时启用 AMD 显卡负载采集
if drm_amdgpu_dep.found()
  daemon_deps  += drm_amdgpu_dep
  daemon_cargs += '-DFMON_HAVE_AMDGPU=1'
endif

executable('fmon-daemon',
  daemon_srcs + common_srcs,
  include_directories : [common_inc, include_directories('.', 'core', 'collectors')],
  dependencies : daemon_deps,
  c_args : daemon_cargs,
  install : true,
  install_dir : get_option('sbindir')
)
