# DePrebuilt: configure-time import of a `kind = "prebuilt"` entry of third_party/vendor.toml
# (DENG-390). Prebuilt binaries (the Slang compiler, DENG-289) are pinned by hash per host and are
# never committed: tools/vendor/vendor.py keeps the archives in its cache ($DE_VENDOR_CACHE, else
# ~/.cache/dengine-vendor) and extracts the host's files into the build directory.
#
#   include(DePrebuilt)
#   de_prebuilt_import(<name> [HOST <host>] [DEST <dir>] [ROOT <repo>] [OPTIONAL])
#
#   HOST      <macos|linux|windows>-<arm64|x64>. Default: the target platform
#             (CMAKE_SYSTEM_NAME + CMAKE_SYSTEM_PROCESSOR), because linked binaries must match the
#             target: windows-xwin-dev on a Mac imports windows-x64.
#   DEST      Output directory, replaced as a whole. Default: <build>/prebuilt/<name>.
#   ROOT      Repository root whose third_party/vendor.toml is read. Default: DE_ROOT_DIR (the
#             self-test points it at a fake repository).
#   OPTIONAL  An entry without an archive for HOST is not an error: <NAME>_FOUND is FALSE. A hash
#             mismatch or a failed download still stops the configure.
#
# Sets in the caller's scope (<NAME> is <name> in upper case):
#   DE_PREBUILT_<NAME>_FOUND  TRUE when the files are in place.
#   DE_PREBUILT_<NAME>_DIR    the output directory. The caller defines its imported targets from
#                             the paths in it (the layout differs per library and host).
#   DE_PREBUILT_<NAME>_HOST   the host that was imported.
#
# The archive is downloaded (with the pinned sha256) only on a cache miss, and the files are
# extracted only when the pin changed or a file is missing (a stamp file in DEST), so a
# re-configure costs a stamp read. vendor.toml is a configure dependency: a pin change re-runs
# the configure. Call it only where a built target links or runs the library (for example under
# DE_BUILD_TOOLS), so a build that does not need it, such as Shipping, never downloads it.

include_guard(GLOBAL)

# The repository this file is in (for vendor.py). A cache variable, because include_guard(GLOBAL)
# skips this line when another directory includes the module again.
get_filename_component(_de_prebuilt_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_DE_PREBUILT_REPO "${_de_prebuilt_repo}" CACHE INTERNAL "DePrebuilt: repository root")
unset(_de_prebuilt_repo)

# de_prebuilt_target_host(<out-var>): the target platform as a vendor.toml host name, or "" when
# it is not one that vendor.py knows.
function(de_prebuilt_target_host out)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(os macos)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(os windows)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(os linux)
    else()
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    set(proc "${CMAKE_SYSTEM_PROCESSOR}")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 proc)
    endif()
    string(TOLOWER "${proc}" proc)
    if(proc MATCHES "^(arm64|aarch64)$")
        set(arch arm64)
    elseif(proc MATCHES "^(x86_64|amd64|x64)$")
        set(arch x64)
    else()
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    set(${out} "${os}-${arch}" PARENT_SCOPE)
endfunction()

function(de_prebuilt_import name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "OPTIONAL" "HOST;DEST;ROOT" "")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "de_prebuilt_import(${name}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    string(TOUPPER "${name}" var)
    set(DE_PREBUILT_${var}_FOUND FALSE PARENT_SCOPE)

    if(arg_ROOT)
        set(root "${arg_ROOT}")
    elseif(DE_ROOT_DIR)
        set(root "${DE_ROOT_DIR}")
    else()
        set(root "${_DE_PREBUILT_REPO}")
    endif()
    if(arg_DEST)
        set(dest "${arg_DEST}")
    else()
        set(dest "${CMAKE_BINARY_DIR}/prebuilt/${name}")
    endif()
    if(arg_HOST)
        set(host "${arg_HOST}")
    else()
        de_prebuilt_target_host(host)
    endif()
    if(host STREQUAL "")
        set(msg "de_prebuilt_import(${name}): target ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR} has no prebuilt host name")
        if(arg_OPTIONAL)
            message(STATUS "${msg}")
            return()
        endif()
        message(FATAL_ERROR "${msg}")
    endif()

    if(NOT Python3_EXECUTABLE)
        find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${root}/third_party/vendor.toml")

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${_DE_PREBUILT_REPO}/tools/vendor/vendor.py"
                --root "${root}" prebuilt "${name}" --host "${host}" --dest "${dest}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if(rc EQUAL 3 AND arg_OPTIONAL)
        # vendor.py exit 3: the entry lists no archive for this host.
        message(STATUS "de_prebuilt_import(${name}): ${err}")
        return()
    endif()
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "de_prebuilt_import(${name}) for host ${host} failed (vendor.py exit ${rc}):\n"
            "${out}\n${err}\n"
            "The archive is pinned in ${root}/third_party/vendor.toml; see tools/vendor/README.md.")
    endif()
    message(STATUS "${out}")

    set(DE_PREBUILT_${var}_FOUND TRUE PARENT_SCOPE)
    set(DE_PREBUILT_${var}_DIR "${dest}" PARENT_SCOPE)
    set(DE_PREBUILT_${var}_HOST "${host}" PARENT_SCOPE)
endfunction()
