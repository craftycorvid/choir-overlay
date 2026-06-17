project('choir', 'cpp',
  version: '0.0.1',
  default_options: ['cpp_std=c++20', 'warning_level=2'])

imgui_dep = dependency('imgui', fallback: ['imgui', 'imgui_dep'])
json_dep  = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])

# Qt6 for the host (Task 11). The state server only needs Core + Network
# (QLocalServer); the tray + settings window add Gui + Widgets.
qt6 = import('qt6')
qt_full_dep = dependency('qt6', modules: ['Core', 'Gui', 'Widgets', 'Network'])
# Lighter dep for the StateServer unit test (no Widgets/Gui needed there).
qt_core_net_dep = dependency('qt6', modules: ['Core', 'Network'])

subdir('src/ipc')
subdir('src/host')
subdir('src/layer')

if get_option('build_tests')
  subdir('tests')
endif
