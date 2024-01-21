project(
  'vacon',
  'c',
  'cpp',
  version: '0.1.0',
  meson_version: '>=1.2.0',
  default_options: [
    'c_std=gnu17',
    'cpp_std=c++20',
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
  ],
  language: ['c', 'cpp'],
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
cmake = import('cmake')

# optional functions

foreach ident : ['pthread_setattr_default_np']
  if cpp.has_function(ident)
    add_project_arguments('-DHAVE_' + ident.to_upper(), language: 'cpp')
  endif
endforeach

# backward, but only if no sanitizers are enabled

if get_option('b_sanitize') == 'none'
  backward = dependency('backward-cpp')
  backward_sources = ['src/backward.cpp']
  add_project_arguments('-DVACON_USE_BACKWARD', language: ['c', 'cpp'])
else
  backward = []
  backward_sources = []
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

# vpl

vpl = dependency('vpl', version: '>2')

# ffmpeg

ffmpeg = [
  dependency('libavcodec'),
  dependency('libavformat'),
  dependency('libavutil'),
]

# v4l2 kernel API

cc.check_header('linux/version.h', required: true)
cc.check_header('linux/videodev2.h', required: true)

# other dependencies

argparse = dependency('argparse', version: '>=3')

fmt = dependency('fmt', version: '>9')

nlohmann_json = dependency('nlohmann_json')

plog = dependency('plog')

readerwriterqueue = dependency('readerwriterqueue')

threads = dependency('threads')

egl = dependency('egl')
if egl.found()
  add_project_arguments('-DHAVE_EGL', language: ['c', 'cpp'])
endif

opengl = dependency('opengl')

sdl3 = dependency('sdl3')

imgui = dependency('imgui', static: true)

# targets

vacon_sources = [
  backward_sources,
  'src/common.cpp',
  'src/linux/camera.cpp',
  'src/linux/camera_frame.cpp',
  'src/linux/encoder.cpp',
  'src/linux/video_frame.cpp',
  'src/linux/video_handler.cpp',
  'src/main.cpp',
  'src/network_handler.cpp',
  'src/rtp_depacketizer.cpp',
  'src/vacon.cpp',
]

vacon_dependencies = [
  argparse,
  backward,
  egl,
  ffmpeg,
  fmt,
  imgui,
  libdatachannel,
  nlohmann_json,
  opengl,
  plog,
  readerwriterqueue,
  sdl3,
  threads,
  vpl,
]

executable('vacon',
  vacon_sources,
  dependencies: vacon_dependencies,
  include_directories: 'src',
  install: true)
