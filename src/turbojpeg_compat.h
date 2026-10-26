#pragma once
#include <turbojpeg.h>

/*
 * The planar-YUV calls this project relies on (tjCompressFromYUVPlanes,
 * tjDecompressToYUVPlanes, tjDecompressHeader3, tjPlaneWidth) are
 * libjpeg-turbo 1.4+, which is the oldest it builds against. One newer
 * call is papered over here: tjGetErrorStr2(), the per-handle error
 * string, is 2.0+. Before that the only error string is tjGetErrorStr()'s
 * single global one -- less precise with two handles erroring at once, but
 * the same text otherwise. TJFLAG_STOPONWARNING arrived in the same 2.0
 * release and, unlike the function, is a macro, so it doubles as the
 * version check.
 */
#ifndef TJFLAG_STOPONWARNING
#define tjGetErrorStr2(handle) ((void)(handle), tjGetErrorStr())
#endif

/*
 * Read-only inputs (the JPEG being decoded, the planes being encoded and
 * their strides) are const in libjpeg-turbo 1.5+ but not in 1.4 -- which
 * never writes through them either -- and nothing in the header tells the
 * two apart. Passing them through void * suits both: it converts
 * implicitly to `unsigned char **` and `const unsigned char **` alike,
 * where no single cast to either does.
 */
#define TJ_SRC(p) ((void *)(p))
