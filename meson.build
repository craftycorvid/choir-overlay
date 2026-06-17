project('choir', 'cpp',
  version: '0.0.1',
  default_options: ['cpp_std=c++20', 'warning_level=2'])

imgui_dep = dependency('imgui', fallback: ['imgui', 'imgui_dep'])
json_dep  = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])

subdir('src/ipc')
subdir('src/host')
subdir('src/layer')

if get_option('build_tests')
  subdir('tests')
endif
