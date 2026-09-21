/*
    config.h for the vendored LAME, in place of the one its configure script
    would write. See cmake/lame.cmake for why LAME is built here at all.

    Deliberately minimal. LAME reads only a handful of symbols from this file
    (STDC_HEADERS, HAVE_STRCHR, HAVE_MEMCPY, HAVE_ERRNO_H, HAVE_FCNTL_H,
    HAVE_STDINT_H, USE_FAST_LOG and the assembly switches), and every one of
    them is answerable the same way on every platform OpenDJ builds for.

    LAME ships configMS.h for exactly this job, but it typedefs the intN_t
    family by hand and #defines them as macros under GCC, which collides with
    <stdint.h> the moment anything else in the translation unit includes it.
    This asks for <stdint.h> instead.
*/

#ifndef OPENDJ_LAME_CONFIG_H
#define OPENDJ_LAME_CONFIG_H

#define STDC_HEADERS 1
#define PROTOTYPES 1

#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1

/* A table lookup for log2 in the psychoacoustic model. Less precise than
   log(), and enough: it is what every stock LAME build uses. */
#define USE_FAST_LOG 1

/* Only ever used under USE_FAST_LOG, and only as a name for `float`. */
typedef float ieee754_float32_t;
typedef double ieee754_float64_t;

/* No HAVE_NASM, no HAVE_XMMINTRIN_H, no MIN_ARCH_SSE: the hand-written i386
   and SSE paths in libmp3lame/i386 and libmp3lame/vector are left out, and
   LAME falls back to the plain C versions of fht and init_xrpow_core. They
   encode a set far faster than real time either way, and skipping them is
   what keeps this to one glob and no assembler.

   No TAKEHIRO_IEEE754_HACK either. It bit-casts floats to integers to
   quantize, which configure only enables after probing the host's float
   layout, and there is no reason to make that bet here.

   No HAVE_MPGLIB, which compiles mpglib_interface.c away to nothing and
   leaves the mpglib decoder out of the build entirely: OpenDJ decodes MP3
   through JUCE and wants LAME only to encode. */

#define LAME_LIBRARY_BUILD 1

#if defined(_MSC_VER)
 #pragma warning(disable : 4305)
#endif

#endif /* OPENDJ_LAME_CONFIG_H */
