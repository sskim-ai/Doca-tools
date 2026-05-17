project('doca-sta-tool', ['c', 'cpp'], version : '0.1.0', default_options : ['warning_level=3'])

cc = meson.get_compiler('c')
doca_inc = include_directories('/opt/mellanox/doca/include')
doca_lib_dir = '/opt/mellanox/doca/lib/' + host_machine.cpu_family() + '-linux-gnu'

doca_common = cc.find_library('doca_common', dirs : doca_lib_dir, required : true)
spdk_inc = include_directories('/usr/local/include', '/usr/include')
spdk_nvme = cc.find_library('spdk_nvme', required : true)
spdk_env_dpdk = cc.find_library('spdk_env_dpdk', required : true)

executable(
  'doca-sta-tool',
  [
    'main_sta_integration.cpp',
    'spdk_backend_qpair_export.c',
    'spdk_doca_queue_export.c',
    'doorbell_polling.c',
  ],
  include_directories : [doca_inc, spdk_inc],
  dependencies : [doca_common, spdk_nvme, spdk_env_dpdk],
  cpp_args : ['-pthread'],
  link_args : ['-pthread'],
  install : false,
)
