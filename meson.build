project(
  'vacon',
  'c',
  'cpp',
  version: '0.1.0',
  meson_version: '>=1.2.0',
  default_options: [
    'b_lto=true',
    'c_std=gnu17',
    'cpp_std=c++20',
    'debug=true',
    'optimization=3',
    'warning_level=2',
    'werror=false',
    'wrap_mode=forcefallback',
  ]
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')
add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'cpp')

cc = meson.get_compiler('c')
cmake = import('cmake')

# egl

cc.check_header('EGL/egl.h', required: true)

# ffmpeg

libavcodec  = dependency('libavcodec')
libavdevice = dependency('libavdevice')
libavformat = dependency('libavformat')
libavutil   = dependency('libavutil')

# glfw

glfw_opts = cmake.subproject_options()
glfw_opts.add_cmake_defines({
  'GLFW_USE_WAYLAND':     true,
  'GLFW_BUILD_DOCS':      false,
  'GLFW_BUILD_EXAMPLES':  false,
  'GLFW_BUILD_TESTS':     false,
})
glfw_cmake = cmake.subproject('glfw', options: glfw_opts)

glfw = glfw_cmake.dependency('glfw')

# libdatachannel

libdatachannel_opts = cmake.subproject_options()
libdatachannel_opts.add_cmake_defines({
  'OPENSSL_USE_STATIC_LIBS': true,
})
libdatachannel_cmake = cmake.subproject('libdatachannel', options: libdatachannel_opts)

#message('Available CMake targets in libdatachannel_cmake:\n - ' + '\n - '.join(libdatachannel_cmake.target_list()))

libdatachannel  = libdatachannel_cmake.dependency('datachannel-static')
juice           = libdatachannel_cmake.dependency('juice-static')
nlohmann_json   = libdatachannel_cmake.dependency('nlohmann_json')
srtp2           = libdatachannel_cmake.dependency('srtp2')
usrsctp         = libdatachannel_cmake.dependency('usrsctp')

# other dependencies

libplacebo_options = {
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
libplacebo = dependency('libplacebo', default_options: libplacebo_options)

nuklear = dependency('nuklear')

readerwriterqueue = dependency('readerwriterqueue')

threads = dependency('threads')

# targets

plplay_sources = [
  'src/plplay/plplay.c',
  'src/plplay/settings.c',
  'src/plplay/ui.c',
  'src/plplay/utils.c',
  'src/plplay/window.c',
  'src/plplay/window_glfw.c',
]

plplay_dependencies = [
  # ffmpeg deps
  libavcodec, libavformat, libavutil,

  glfw,
  libplacebo,
  nuklear,
  threads,
]

executable('plplay',
  plplay_sources,
  c_args: [
    '-DHAVE_EGL',
    '-DHAVE_GLFW_OPENGL',
    '-DHAVE_NUKLEAR',
    '-DPL_HAVE_PTHREAD',
    '-DPTHREAD_HAS_SETCLOCK',
    '-DUSE_GL',
  ],
  dependencies: plplay_dependencies)

vacon_sources = [
  'src/main.cpp',
  'src/camera_sender.cpp',
  'src/decoder_receiver.cpp',
  'src/sink_receiver.cpp',
  'src/v4l2_ffmpeg_input_vaapi_encode.c',
]

vacon_dependencies = [
  # libdatachannel deps
  libdatachannel, juice, nlohmann_json, srtp2, usrsctp,

  # ffmpeg deps
  libavcodec, libavdevice, libavformat, libavutil,

  # other deps
  libplacebo,
  readerwriterqueue,
  threads,
]

executable('vacon',
  vacon_sources,
  dependencies: vacon_dependencies,
  install: true)
