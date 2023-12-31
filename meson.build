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
    'default_library=static',
    'optimization=3',
    'warning_level=2',
    'werror=false',
    'wrap_mode=forcefallback',
    'glfw:display-api=wayland',
  ]
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: ['c', 'cpp'])

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
cmake = import('cmake')

# optional functions

foreach ident : ['pthread_setattr_default_np']
  if cpp.has_function(ident)
    add_project_arguments('-DHAVE_' + ident.to_upper(), language: 'cpp')
  endif
endforeach

# libdatachannel

libdatachannel_opts = cmake.subproject_options()
if get_option('default_library') == 'static'
  libdatachannel_opts.add_cmake_defines({
    'OPENSSL_USE_STATIC_LIBS': true,
  })
endif
libdatachannel_cmake = cmake.subproject('libdatachannel', options: libdatachannel_opts)

libdatachannel = [
  libdatachannel_cmake.dependency('datachannel-static'),
  libdatachannel_cmake.dependency('juice-static'),
  libdatachannel_cmake.dependency('nlohmann_json'),
  libdatachannel_cmake.dependency('srtp2'),
  libdatachannel_cmake.dependency('usrsctp'),
]

# ffmpeg

ffmpeg = [
  dependency('libavcodec'),
  dependency('libavdevice'),
  dependency('libavformat'),
  dependency('libavutil'),
]

# glfw

cc.check_header('EGL/egl.h', required: true) # For GLFW_EXPOSE_NATIVE_EGL

glfw = dependency('glfw3', required: false)
if not glfw.found()
  # Fallback to the wrap.
  glfw = dependency('glfw3')
endif

# other dependencies

argparse = dependency('argparse', version: '>=3')

backward = dependency('backward-cpp')

fmt = dependency('fmt', version: '>9', static: true)

libplacebo = dependency('libplacebo',
  default_options: {
    'default_library':  'static',
    'opengl':           'enabled',
    'demos':            false,
    'd3d11':            'disabled',
    'dovi':             'disabled',
    'glslang':          'disabled',
    'lcms':             'disabled',
    'libdovi':          'disabled',
    'unwind':           'disabled',
    'vk-proc-addr':     'disabled',
    'vulkan':           'disabled',
    'xxhash':           'disabled',
  }
)

nlohmann_json = dependency('nlohmann_json', required: false)
if not nlohmann_json.found()
  # Fallback to the wrap.
  nlohmann_json = dependency('nlohmann_json')
endif

nuklear = dependency('nuklear')

plog = dependency('plog')

readerwriterqueue = dependency('readerwriterqueue')

threads = dependency('threads')

# targets

plplay_static = static_library('plplay',
  sources: [
    'src/plplay/plplay.c',
    'src/plplay/settings.c',
    'src/plplay/ui.c',
    'src/plplay/window.c',
    'src/plplay/window_glfw.c',
  ],
  c_args: [
    '-DHAVE_EGL',
    '-DHAVE_GLFW_OPENGL',
    '-DHAVE_NUKLEAR',
    '-DPL_HAVE_PTHREAD',
    '-DPTHREAD_HAS_SETCLOCK',
    '-DUSE_GL',
  ],
  dependencies: [
    ffmpeg,
    glfw,
    libplacebo,
    nuklear,
    threads,
  ],
)
plplay = declare_dependency(link_with: plplay_static)

vacon_sources = [
  'src/backward.cpp',
  'src/camera_encoder.cpp',
  'src/common.cpp',
  'src/main.cpp',
  'src/network_handler.cpp',
  'src/rtp_depacketizer.cpp',
  #'src/camera_sender.cpp',
  #'src/decoder_receiver.cpp',
  #'src/sink_receiver.cpp',
  #'src/v4l2_ffmpeg_input_vaapi_encode.c',
  #'src/video_receiver.cpp',
]

vacon_dependencies = [
  argparse,
  backward,
  ffmpeg,
  fmt,
  libdatachannel,
  libplacebo,
  nlohmann_json,
  plog,
  plplay,
  readerwriterqueue,
  threads,
]

executable('vacon',
  vacon_sources,
  dependencies: vacon_dependencies,
  install: true)
