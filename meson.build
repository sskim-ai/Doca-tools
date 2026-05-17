project('doca-project', 'c', version : '0.1.0', default_options : ['warning_level=3'])

cc = meson.get_compiler('c')

doca_inc = include_directories('/opt/mellanox/doca/include')

doca_lib_dir = '/opt/mellanox/doca/lib/' + host_machine.cpu_family() + '-linux-gnu'

doca_common = cc.find_library('doca_common', dirs : doca_lib_dir, required : false)

executable(
  'doca-project',
  'src/main.c',
  include_directories : doca_inc,
  dependencies : doca_common.found() ? [doca_common] : [],
  install : false,
)
