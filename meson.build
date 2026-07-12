project(
    'electric_eye',
    'c',
    version: '0.0.1',
    default_options: [
        'warning_level=3',
        'c_std=c23',
        'buildtype=debugoptimized',
    ],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

core_src = [
    'src/core/eeye.c',
    'src/core/conv.c',
    'src/core/frame_ring_buffer.c',
    'src/core/video_frame.c',
    'src/core/point_opps.c',
]
# core_lib = static_library(
#     'eeye-core',
#     #dependencies: [dependency(libyuv)]
# )
#
core_dep = declare_dependency(
    # link_with: core_lib,
    include_directories: include_directories('src/core'),
)

executable('eeye', sources: core_src, dependencies: m_dep)

decoder_src = ['src/decoder/decoder.c']
ffmpeg_deps = [
    dependency('libavcodec'),
    dependency('libavformat'),
    dependency('libavutil'),
]

decoder_lib = static_library(
    'videodecoder',
    decoder_src,
    dependencies: ffmpeg_deps,
    include_directories: include_directories('src/decoder'),
)

executable(
    'live_processor',
    'src/main.c',
    link_with: [decoder_lib],
    dependencies: [core_dep, ffmpeg_deps],
)
