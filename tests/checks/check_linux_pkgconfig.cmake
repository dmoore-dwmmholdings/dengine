if(NOT DE_TOOLCHAIN OR NOT DE_TEST_DIR)
    message(FATAL_ERROR "toolchain and test directory are required")
endif()
find_program(_pkgconfig NAMES pkg-config pkgconf)
if(NOT _pkgconfig)
    message("linux_pkgconfig_isolation: SKIPPED (pkg-config unavailable)")
    return()
endif()
file(REMOVE_RECURSE "${DE_TEST_DIR}")
set(_root "${DE_TEST_DIR}/sysroot")
set(_host "${DE_TEST_DIR}/host-pkgconfig")
file(MAKE_DIRECTORY "${_root}/usr/include"
    "${_root}/usr/lib/x86_64-linux-gnu/pkgconfig" "${_host}" "${DE_TEST_DIR}/probe"
    "${DE_TEST_DIR}/host-prefix/lib/pkgconfig")
file(WRITE "${_root}/usr/lib/x86_64-linux-gnu/pkgconfig/dengine-target.pc" [=[
prefix=/usr
Name: dengine-target
Description: target dependency probe
Version: 1.0
Libs: -L${prefix}/lib -ldengine_target
Cflags: -I${prefix}/include/dengine-target
]=])
file(WRITE "${_host}/dengine-host.pc" [=[
Name: dengine-host
Description: forbidden host dependency probe
Version: 1.0
Libs: -L/host-only -ldengine_host
Cflags: -I/host-only
]=])
file(COPY_FILE "${_host}/dengine-host.pc"
    "${DE_TEST_DIR}/host-prefix/lib/pkgconfig/dengine-host-prefix.pc")
file(WRITE "${DE_TEST_DIR}/probe/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(pkgconfig_probe NONE)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TARGET_DEP REQUIRED dengine-target)
pkg_check_modules(HOST_DEP QUIET dengine-host)
pkg_check_modules(HOST_PREFIX_DEP QUIET dengine-host-prefix)
if(HOST_DEP_FOUND OR HOST_PREFIX_DEP_FOUND)
    message(FATAL_ERROR "host pkg-config dependency leaked into target build")
endif()
if(NOT TARGET_DEP_INCLUDE_DIRS STREQUAL "${CMAKE_SYSROOT}/usr/include/dengine-target")
    message(FATAL_ERROR "target metadata was not resolved inside the sysroot: ${TARGET_DEP_INCLUDE_DIRS}")
endif()
]=])
set(ENV{PKG_CONFIG_PATH} "${_host}")
set(ENV{PKG_CONFIG_LIBDIR} "${_host}")
execute_process(COMMAND "${CMAKE_COMMAND}" -G Ninja
    -S "${DE_TEST_DIR}/probe" -B "${DE_TEST_DIR}/build"
    "-DCMAKE_TOOLCHAIN_FILE=${DE_TOOLCHAIN}" "-DDE_LINUX_SYSROOT=${_root}"
    "-DCMAKE_PREFIX_PATH=${DE_TEST_DIR}/host-prefix"
    "-DPKG_CONFIG_EXECUTABLE=${_pkgconfig}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "pkg-config isolation failed:\n${_out}\n${_err}")
endif()
message(STATUS "linux_pkgconfig_isolation: target found, host excluded")
