project(
  'imgui',
  'cpp',
  license: 'MIT',
  version: '1.90.1',
  meson_version: '>=0.50.0',
)

if host_machine.system() == 'darwin'
  add_languages('objcpp')
endif

include_dirs = include_directories('.', 'backends')
sources = files(
  'misc/cpp/imgui_stdlib.cpp',
  'imgui.cpp',
  'imgui_demo.cpp',
  'imgui_draw.cpp',
  'imgui_tables.cpp',
  'imgui_widgets.cpp',
)

cpp = meson.get_compiler('cpp')
dependencies = []

# renderer backends
metal_dep = dependency('appleframeworks', modules: ['Foundation', 'AppKit', 'GameController', 'Metal'], required: get_option('metal'))
if metal_dep.found()
  sources += 'backends/imgui_impl_metal.mm'
  dependencies += metal_dep
endif
libgl_dep = dependency('opengl', required: get_option('opengl'))
if libgl_dep.found()
  sources += 'backends/imgui_impl_opengl3.cpp'
  dependencies += libgl_dep
  dependencies += cpp.find_library('dl', required: false)
endif
sdl3_renderer_dep = dependency('sdl3', version: '>=3.0.0', required: get_option('sdl_renderer'))
if sdl3_renderer_dep.found()
  sources += 'backends/imgui_impl_sdlrenderer3.cpp'
  dependencies += sdl3_renderer_dep
endif

# platform backends
sdl3_dep = dependency('sdl3', required: get_option('sdl3'))
if sdl3_dep.found()
  sources += 'backends/imgui_impl_sdl3.cpp'
  dependencies += sdl3_dep
endif
osx_dep = dependency('appleframeworks', modules: ['Carbon', 'Cocoa', 'GameController'], required: get_option('osx'))
if osx_dep.found()
  sources += 'backends/imgui_impl_osx.mm'
endif

api = '-DIMGUI_API=__attribute__((visibility("default")))'

imgui = library(
  'imgui',
  sources,
  cpp_args: api,
  gnu_symbol_visibility: 'hidden',
  dependencies: dependencies,
  include_directories: include_dirs,
)

imgui_dep = declare_dependency(
  compile_args: api,
  include_directories: include_dirs,
  link_with: imgui,
)
