project(
  'vacon',
  'c',
  'cpp',
  version: '0.1.0',
  meson_version: '>=1.2.0',
  default_options: [
    'c_std=gnu17',
    'cpp_std=c++23',
    'debug=true',
    'optimization=3',
    'warning_level=2',
    'werror=false',
  ]
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

add_project_arguments([
  '-D_POSIX_C_SOURCE=200809L',
  '-DPROJECT_NAME="@0@"'.format(meson.project_name()),
  '-DPROJECT_VERSION="@0@"'.format(meson.project_version()),
  ],
  language: ['c', 'cpp'])

if get_option('debug')
  add_global_arguments(cpp.get_supported_arguments([
    '-ggdb3',
    '-fno-omit-frame-pointer',
    ]),
    language: ['c', 'cpp'])
endif

if get_option('onevpl_priority_path') != ''
  add_project_arguments([
    '-DVACON_ONEVPL_PRIORITY_PATH="@0@"'.format(get_option('onevpl_priority_path'))
    ],
    language: ['c', 'cpp'])
endif

os_deps = []

# Linux-specific dependencies

if host_machine.system() == 'linux'
  # fontconfig
  fontconfig = dependency('fontconfig')
  os_deps += fontconfig

  # OpenGL
  add_project_arguments(['-DGL_GLEXT_PROTOTYPES'], language: ['c', 'cpp'])
  egl = dependency('egl', version: '>=1.5')
  gl = dependency('gl')
  opengl = dependency('opengl')
  os_deps += [egl, gl, opengl]

  # VAAPI
  va = dependency('libva')
  os_deps += va
  va_wayland = dependency('libva-wayland')
  os_deps += va_wayland

  # V4L2 kernel API
  cc.check_header('linux/version.h', required: true)
  cc.check_header('linux/videodev2.h', required: true)

  # VPL (Intel Video Processing Library)
  vpl = dependency('vpl', version: '>2')
  os_deps += vpl
  add_project_arguments(['-DONEVPL_EXPERIMENTAL'], language: ['c', 'cpp'])

  # Wayland
  wayland_client = dependency('wayland-client')
  os_deps += wayland_client
else
  error('Only Linux is supported')
endif

# optional functions

foreach ident : ['pthread_setattr_default_np']
  if cpp.has_function(ident)
    add_project_arguments('-DHAVE_' + ident.to_upper(), language: 'cpp')
  endif
endforeach

# backward, but only if no sanitizers are enabled

if get_option('b_sanitize') == 'none'
  backward = dependency('backward-cpp')
  add_project_arguments('-DVACON_USE_BACKWARD', language: ['c', 'cpp'])
else
  backward = []
endif

# other dependencies

argparse = dependency('argparse', version: '>=3')
imgui = dependency('imgui', static: true)
libdatachannel = dependency('LibDataChannel')
libhydrogen = dependency('libhydrogen')
nlohmann_json = dependency('nlohmann_json')
plog = dependency('plog')
readerwriterqueue = dependency('readerwriterqueue')
sdl3 = dependency('sdl3')
threads = dependency('threads')

# targets

vacon_sources = [
  'src/app.cpp',
  'src/args.cpp',
  'src/event.cpp',
  'src/invite.cpp',
  'src/linux/camera.cpp',
  'src/linux/decoder.cpp',
  'src/linux/encoder.cpp',
  'src/linux/font.cpp',
  'src/linux/mfx.cpp',
  'src/linux/mfx_loader.cpp',
  'src/network_handler.cpp',
  'src/rtc_utils.cpp',
  'src/rtp/generic_packetizer.cpp',
  'src/rtp/generic_depacketizer.cpp',
  'src/sdl.cpp',
  'src/sdlmain.cpp',
  'src/ui.cpp',
  'src/util.cpp',
]

vacon_dependencies = [
  argparse,
  backward,
  imgui,
  libdatachannel,
  libhydrogen,
  nlohmann_json,
  os_deps,
  plog,
  readerwriterqueue,
  sdl3,
  threads,
]

executable('vacon',
  vacon_sources,
  dependencies: vacon_dependencies,
  include_directories: 'src',
  install: true)
