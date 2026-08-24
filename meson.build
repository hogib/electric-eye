project(
    'electric_eye',
    'c',
    version: '0.0.1',
    default_options: [
        'warning_level=3',
        'c_std=gnu23',
        'buildtype=debugoptimized',
        # electric_eye itself has no library() targets, so this only matters
        # when the libturbojpeg fallback below pulls in libjpeg-turbo (and
        # its own spng/zlib subprojects) from source -- meson propagates a
        # project's default_library down through subprojects unless a wrap
        # overrides it, so setting it here is what makes that whole chain
        # link statically instead of just the outermost one.
        'default_library=static',
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
#
# stream_server.c only calls tjInitCompress/tjCompressFromYUVPlanes/tjBufSize
# -- no file I/O, no decode. Prefer the system libturbojpeg (usually a distro
# dev package) when it's there; if it's missing or broken (this fell over on
# a fresh Linux Mint box), fall back to building libjpeg-turbo from source as
# a static subproject, so eeye needs no turbojpeg apt package and no
# libturbojpeg.so.0 at runtime either way. This is "no system package
# needed", not "fully static ELF" -- glibc/pthread/OpenMP are still linked
# dynamically.
turbojpeg_dep = dependency(
    'libturbojpeg',
    fallback: ['libjpeg-turbo', 'turbojpeg_dep'],
    default_options: ['turbojpeg=enabled'],
)

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
    'src/jpeg_decode.c',
    'src/video_frame.c',
    'src/point_opps.c',
    'src/rpicam_in.c',
    'src/stream_server.c',
    'src/v4l2_in.c',
    'src/v4l2_out.c',
    'src/video_threads.c',
    'src/virtual_cam.c',
]

dep = declare_dependency(
    include_directories: include_directories('src/'),
)

executable(
    exe_name,
    sources: src,
    dependencies: [m_dep, turbojpeg_dep, omp_dep, thread_dep],
)

# Unit tests. Each includes the translation unit it exercises directly
# (rather than linking against it) so the file-static helpers -- the config
# parser, the JPEG frame splitter, the negotiation ranker -- are reachable
# without widening their visibility in the shipping build just to test them.
#
# Run with: meson test -C builddir
test_deps = [m_dep, turbojpeg_dep, omp_dep, thread_dep]
test_inc = include_directories('src', 'tests')

# Extra translation units each test needs beyond the one it includes:
# rpicam_in.c and v4l2_in.c both reference the shared JPEG decoder and the
# frame pool, so those get linked in alongside.
test_extra_sources = {
    'config': [],
    'ring_buffer': [],
    'jpeg_framing': ['src/jpeg_decode.c', 'src/video_frame.c'],
    'negotiation': ['src/jpeg_decode.c', 'src/video_frame.c'],
    'jpeg_decode': ['src/jpeg_decode.c', 'src/video_frame.c'],
    'log_edges': ['src/conv.c', 'src/video_frame.c'],
    'effect_chain': [
        'src/conv.c',
        'src/point_opps.c',
        'src/video_frame.c',
        'src/config.c',
    ],
}

foreach t, extra : test_extra_sources
    test_exe = executable(
        'test_' + t,
        sources: ['tests/test_' + t + '.c'] + extra,
        include_directories: test_inc,
        dependencies: test_deps,
        build_by_default: false,
    )
    test(t, test_exe)
endforeach




