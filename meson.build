project('fmon', 'c',
  version : '1.3.5',
  license : 'BSD-2-Clause',
  meson_version : '>= 0.60',
  default_options : [
    'c_std=c11',
    'warning_level=3',
    'optimization=2',
    'buildtype=release',
    'b_pie=true',
  ]
)

cc = meson.get_compiler('c')
thread_dep  = dependency('threads')
gtk_dep     = dependency('gtk4')
cairo_dep   = dependency('cairo')
fontconfig_dep = dependency('fontconfig')
x11_dep     = dependency('x11')
i18n        = import('i18n')

kvm_dep     = cc.find_library('kvm')
m_dep       = cc.find_library('m')
devstat_dep = cc.find_library('devstat')
cam_dep     = cc.find_library('cam')

# 可选：AMD 显卡负载（libdrm_amdgpu）。有则启用真实核显/独显监控。
drm_amdgpu_dep = dependency('libdrm_amdgpu', required : false)

# 统一的编译选项：把“能提前发现的错误”都变成警告
add_project_arguments(
  '-D_FORTIFY_SOURCE=2',
  '-fstack-protector-strong',
  '-Wformat=2',
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Wwrite-strings',
  '-Wvla',
  '-Wno-unused-parameter',
  language : 'c'
)

# 链接期加固：立即绑定符号 + 只读重定位 + 不可执行栈
add_project_link_arguments(
  '-Wl,-z,relro',
  '-Wl,-z,now',
  '-Wl,-z,noexecstack',
  language : 'c'
)

common_inc  = include_directories('src/common')
common_srcs = files(
  'src/common/fmon_shm.c',
  'src/common/fmon_child.c',
  'src/common/fmon_smart.c'
)

# 把安装前缀传给程序：所有“可能变化”的路径都由前缀派生，
# 不写死 /usr/local（基础系统路径 /bin、/sbin 等保持原样）。
add_project_arguments('-DFMON_LOCALBASE="' + get_option('prefix') + '"',
                      language : 'c')

subdir('src/daemon')
subdir('src/gui')
subdir('po')
subdir('tests')

summary({
  'AMD GPU sensor (libdrm_amdgpu)' : drm_amdgpu_dep.found(),
}, section : 'Features')

# 许可证（安装到 FreeBSD 惯例位置，跟随安装前缀）
install_data(
  ['LICENSE', 'THIRD-PARTY-LICENSES.md'],
  install_dir : join_paths(get_option('datadir'), 'licenses', 'fmon')
)

# FreeBSD rc.d 启动脚本（@PREFIX@ 在 meson 配置阶段替换）
rc_conf = configuration_data()
rc_conf.set('PREFIX', get_option('prefix'))
rc_script = configure_file(
  input : 'rc.d/fmon_daemon.in',
  output : 'fmon_daemon',
  configuration : rc_conf
)

install_data(rc_script,
  install_dir : join_paths(get_option('sysconfdir'), 'rc.d'),
  install_mode : 'rwxr-xr-x'
)
