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
ffmpeg_deps = [
    dependency('libavcodec', required: false),
    dependency('libavformat', required: false),
    dependency('libavutil', required: false),
]

src = [
    'src/eeye.c',
    'src/conv.c',
    'src/frame_ring_buffer.c',
    'src/video_frame.c',
    'src/point_opps.c',
    'src/video_threads.c',
]

dep = declare_dependency(
    include_directories: include_directories('src/'),
)

executable('eeye', sources: src, dependencies: [m_dep, ffmpeg_deps])




