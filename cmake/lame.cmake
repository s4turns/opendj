# LAME, compiled into OpenDJ so the recorder can write MP3.
#
# Built here rather than found for the same reason Rubber Band is: it ships
# autotools, only Linux distributions package it, and every platform should get
# the same version. LGPL 2.1 or later, so licence compatible.
#
# A non-recursive glob over libmp3lame/*.c is the whole source list.
# libmp3lame/i386 and libmp3lame/vector hold the hand-written assembly and SSE
# paths, and cmake/lame-config.h deliberately switches those off, so leaving
# them out of the glob is not an omission. mpglib_interface.c is in the glob but
# compiles to nothing without HAVE_MPGLIB, which is why the mpglib decoder is
# not here either.

file(GLOB OPENDJ_LAME_SOURCES CONFIGURE_DEPENDS "${lame_SOURCE_DIR}/libmp3lame/*.c")

add_library(opendj_lame STATIC ${OPENDJ_LAME_SOURCES})

# LAME asks for <config.h> by that name, so ours is copied in under it rather
# than included by path.
configure_file("${CMAKE_SOURCE_DIR}/cmake/lame-config.h"
               "${CMAKE_BINARY_DIR}/lame-config/config.h" COPYONLY)

target_compile_definitions(opendj_lame PRIVATE HAVE_CONFIG_H=1)

target_include_directories(opendj_lame PRIVATE
    "${CMAKE_BINARY_DIR}/lame-config"
    "${lame_SOURCE_DIR}/libmp3lame")

# lame.h, and nothing else, is what the rest of OpenDJ sees.
target_include_directories(opendj_lame SYSTEM PUBLIC "${lame_SOURCE_DIR}/include")

target_compile_options(opendj_lame PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
set_target_properties(opendj_lame PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_compile_definitions(opendj_lame PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
endif()
