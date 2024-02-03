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

add_global_arguments([
  '-fno-omit-frame-pointer',
  ],
  language: ['c', 'cpp'],
)

add_project_arguments([
  '-D_POSIX_C_SOURCE=200809L',
  '-DPROJECT_NAME="@0@"'.format(meson.project_name()),
  '-DPROJECT_VERSION="@0@"'.format(meson.project_version()),
  ],
  language: ['c', 'cpp'],
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
cmake = import('cmake')

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

  # V4L2 kernel API
  cc.check_header('linux/version.h', required: true)
  cc.check_header('linux/videodev2.h', required: true)

  # VPL (Intel Video Processing Library)
  vpl = dependency('vpl', version: '>2')
  os_deps += vpl
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

# libdatachannel

libdatachannel = dependency('LibDataChannel', required: false)
if not libdatachannel.found()
  libdatachannel_opts = cmake.subproject_options()
  libdatachannel_opts.set_install(false)
  libdatachannel_cmake = cmake.subproject('libdatachannel', options: libdatachannel_opts)

  libdatachannel = [
    libdatachannel_cmake.dependency('datachannel-static'),
    libdatachannel_cmake.dependency('juice-static'),
    libdatachannel_cmake.dependency('srtp2'),
    libdatachannel_cmake.dependency('usrsctp'),
  ]
endif

# ffmpeg

ffmpeg = [
  dependency('libavcodec'),
  dependency('libavformat'),
  dependency('libavutil'),
]


# other dependencies

argparse = dependency('argparse', version: '>=3')
imgui = dependency('imgui', static: true)
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
  'src/linux/camera.cpp',
  'src/linux/encoder.cpp',
  'src/linux/video_handler.cpp',
  'src/network_handler.cpp',
  'src/rtp_depacketizer.cpp',
  'src/sdl.cpp',
  'src/sdlmain.cpp',
  'src/ui.cpp',
  'src/util.cpp',
]

vacon_dependencies = [
  argparse,
  backward,
  ffmpeg,
  imgui,
  libdatachannel,
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
