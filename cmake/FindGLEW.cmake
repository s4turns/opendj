# Not a search.
#
# GLEW is built from source in the top level CMakeLists.txt, next to projectM
# itself, and GLEW::glew is defined there. This module exists only so that
# projectM's own find_package(GLEW REQUIRED) finds what is already present
# instead of looking for an installed copy that a machine without vcpkg does
# not have.
#
# It has to shadow CMake's own FindGLEW, which it does by being on
# CMAKE_MODULE_PATH: find_package tries module mode first. CMake's module would
# be wrong twice over here -- it defines GLEW::GLEW, where projectM tests for
# the lower case GLEW::glew, and it looks for a library file that does not
# exist until the build runs.

if(TARGET GLEW::glew)
    set(GLEW_FOUND TRUE)
    set(GLEW_LIBRARIES GLEW::glew)
    set(GLEW_INCLUDE_DIRS "")
else()
    set(GLEW_FOUND FALSE)

    if(GLEW_FIND_REQUIRED)
        message(FATAL_ERROR
                "GLEW::glew was expected to be defined before find_package(GLEW). "
                "It is built in the top level CMakeLists.txt; see the note there.")
    endif()
endif()
