project(
    'electric_eye',
    'c',
    version: '0.0.1',
    default_options: [
        'warning_level=3',
        'c_std=gnu23',
        'buildtype=debugoptimized',
    ],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

# frame_ring_buffer.c now uses pthread_mutex_t/sem_t directly (previously
# pthread linkage was only ever pulled in incidentally through
# pthread_create() elsewhere) -- declare it explicitly rather than rely on
# whichever glibc happens to already fold pthread into libc.
thread_dep = dependency('threads')

# Both the capture side (v4l2_in.c) and the output side (v4l2_out.c) now
# talk to /dev/videoN directly via V4L2 ioctls and MMAP streaming -- the
# ffmpeg CLI subprocess this project used to shell out to for both ends is
# gone. Nothing in src/ has ever linked against libavcodec/libavformat/
# libavutil directly (grep confirms it), so the dependency that used to be
# declared here for them is dropped rather than kept around unused.
turbojpeg_dep = dependency('libturbojpeg')

# point_opps.c and conv.c already carry #pragma omp parallel for on every
# hot loop -- those loops were written to be race-free specifically so this
# would be safe once wired up (see the raw/work split: source and
# destination buffers are always disjoint). Without this dependency, every
# one of those pragmas is silently ignored and the loops run single-threaded,
# which is the state the project has been in until now. required: false
# matches ffmpeg_deps above: still build on a host without an OpenMP
# runtime, just without the parallelism.
omp_dep = dependency('openmp', required: false)
if not omp_dep.found()
    warning(
        'OpenMP not found: point_opps.c/conv.c will build but their #pragma omp loops will run single-threaded.',
    )
endif

exe_name = 'eeye'

if get_option('buildtype') == 'debug'
    exe_name = 'eeye-d'
endif

if get_option('buildtype') == 'release'
    exe_name = 'eeye'
endif


src = [
    'src/eeye.c',
    'src/config.c',
    'src/conv.c',
    'src/effect_chain.c',
    'src/frame_ring_buffer.c',
    'src/video_frame.c',
    'src/point_opps.c',
    'src/v4l2_in.c',
    'src/v4l2_out.c',
    'src/video_threads.c',
]

dep = declare_dependency(
    include_directories: include_directories('src/'),
)

executable(
    exe_name,
    sources: src,
    dependencies: [m_dep, turbojpeg_dep, omp_dep, thread_dep],
)




