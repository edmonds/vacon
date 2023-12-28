project('backward-cpp', 'cpp', version: '1.6')

cpp = meson.get_compiler('cpp')

libelf = dependency('libelf', required: true)

libdwarf = cpp.find_library(
  'dwarf',
  has_headers: ['libdwarf/libdwarf.h'],
  required: true,
)

libunwind = dependency('libunwind', required: true)

deps = [ libelf, libdwarf, libunwind ]

backward_dep = declare_dependency(
  dependencies: deps,
  include_directories: include_directories('.'),
)
