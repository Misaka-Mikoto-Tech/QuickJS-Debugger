cmake_minimum_required(VERSION 3.10)
project(qjs_debugger LANGUAGES C CXX)

enable_testing()

# MSVC < 17.5 (cl < 19.35) does not ship <stdatomic.h> for C.
# Inject a shim directory so that #include <stdatomic.h> in QuickJS
# picks up our compatibility header instead of failing.
if(MSVC AND (MSVC_VERSION LESS 1935))
    message(STATUS "MSVC ${MSVC_VERSION} lacks C11 <stdatomic.h> – using compat shim")
    include_directories(BEFORE SYSTEM "${CMAKE_SOURCE_DIR}/compat/msvc")
endif()

add_subdirectory(quickjs)
add_subdirectory(debugger)
