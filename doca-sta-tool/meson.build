project('doca-sta-tool', ['c', 'cpp'], version : '0.1.0', default_options : ['warning_level=3'])

cc = meson.get_compiler('c')
doca_inc = include_directories('/opt/mellanox/doca/include')
doca_lib_dir = '/opt/mellanox/doca/lib/' + host_machine.cpu_family() + '-linux-gnu'
spdk_root = get_option('spdk_root')
spdk_inc_dir = spdk_root / 'include'
spdk_internal_inc_dir = spdk_root / 'lib' / 'nvme'
spdk_pkgconfig_dir = get_option('spdk_pkgconfig_dir')
spdk_dpdk_lib_dir = get_option('spdk_dpdk_lib_dir')
spdk_dpdk_tmp_lib_dir = get_option('spdk_dpdk_tmp_lib_dir')
mellanox_dpdk_lib_dir = get_option('mellanox_dpdk_lib_dir')

doca_common = cc.find_library('doca_common', dirs : doca_lib_dir, required : true)
openssl_ssl = cc.find_library('ssl', required : true)
openssl_crypto = cc.find_library('crypto', required : true)
libuuid = cc.find_library('uuid', required : true)
spdk_inc = include_directories(spdk_inc_dir, spdk_internal_inc_dir)

spdk_pkg_env = environment()
spdk_pkg_env.prepend('PKG_CONFIG_PATH', spdk_pkgconfig_dir)

spdk_cflags_cmd = run_command(
  'pkg-config',
  '--cflags',
  'spdk_nvme',
  env : spdk_pkg_env,
  check : false,
)
if spdk_cflags_cmd.returncode() != 0
  error('Failed to resolve SPDK cflags via pkg-config for spdk_nvme')
endif

spdk_link_cmd = run_command(
  'pkg-config',
  '--libs',
  '--static',
  'spdk_nvme',
  'spdk_env_dpdk',
  env : spdk_pkg_env,
  check : false,
)
if spdk_link_cmd.returncode() != 0
  error('Failed to resolve SPDK static link flags via pkg-config for spdk_nvme/spdk_env_dpdk')
endif

spdk_cflags = spdk_cflags_cmd.stdout().strip().split()
spdk_link_args = spdk_link_cmd.stdout().strip().split()
runtime_lib_dirs = []
runtime_rpath = ''

foreach arg : spdk_link_args
  if arg.startswith('-L')
    runtime_lib_dirs += [arg.substring(2)]
  endif
endforeach

if runtime_lib_dirs.length() > 0
  runtime_rpath = runtime_lib_dirs[0]
  if runtime_lib_dirs.length() > 1
    runtime_rpath += ':' + runtime_lib_dirs[1]
  endif
  if runtime_lib_dirs.length() > 2
    runtime_rpath += ':' + runtime_lib_dirs[2]
  endif
  if runtime_lib_dirs.length() > 3
    runtime_rpath += ':' + runtime_lib_dirs[3]
  endif
  if runtime_lib_dirs.length() > 4
    runtime_rpath += ':' + runtime_lib_dirs[4]
  endif
endif

if runtime_rpath == ''
  runtime_rpath = spdk_dpdk_lib_dir
else
  runtime_rpath += ':' + spdk_dpdk_lib_dir
endif

runtime_rpath += ':' + spdk_dpdk_tmp_lib_dir
runtime_rpath += ':' + mellanox_dpdk_lib_dir

executable(
  'doca-sta-tool',
  [
    'main_sta_integration.cpp',
    'spdk_backend_qpair_export.c',
    'spdk_doca_queue_export.c',
    'doorbell_polling.c',
  ],
  include_directories : [doca_inc, spdk_inc],
  dependencies : [doca_common, openssl_ssl, openssl_crypto, libuuid],
  c_args : spdk_cflags,
  cpp_args : ['-pthread'] + spdk_cflags,
  link_args : ['-pthread'] + spdk_link_args,
  build_rpath : runtime_rpath,
  install_rpath : runtime_rpath,
  install : false,
)
