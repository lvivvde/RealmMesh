# Ninja generator gate and link pool (#123).
#
# realmmesh_check_ninja(<generator> <program> <out_error>)
#
#   Sets <out_error> to an empty string when <generator> is not a Ninja
#   generator, or when <program> runs and reports Ninja >= 1.11 (the first
#   release with `ninja -t missingdeps`, used to audit the generated-file
#   dependencies of the build graph).
#   Otherwise sets it to a message naming the problem and the two ways out:
#   install Ninja, or pick the Make fallback preset explicitly. The generator is
#   never switched behind the developer's back.
#
# realmmesh_require_ninja()
#
#   Call before project(). Resolves the ninja CMake will use (CMAKE_MAKE_PROGRAM,
#   else the same names CMake searches) and stops the configure with the
#   message above when it is missing or too old. Before project() CMake has not
#   looked for ninja yet, so its own "unable to find a build program" error
#   would otherwise come first and carry no hint.
#
# realmmesh_use_ninja_link_pool()
#
#   Under Ninja, puts every link step (static archives included) into the
#   depth-1 pool REALMMESH_NINJA_LINK_POOL, so links never run beside each
#   other while compiles keep the full --jobs budget. Call after project() and
#   before the first target is added: CMAKE_JOB_POOL_LINK only initializes
#   targets created after it is set. A no-op for other generators.
#
# realmmesh_link_outside_pool(<target>)
#
#   Takes one target's link step back out of that pool. Meant for test
#   executables only: gtest_discover_tests runs the fresh binary as a POST_BUILD
#   step inside the same link edge, and on macOS the first exec of a newly
#   linked binary waits about 0.5 s, so a depth-1 pool serialized those waits
#   across every relinked test (each edge ~0.5 s, 43-73 edges per header or
#   proto change; Make overlaps them with compiles). Libraries and production
#   executables stay pooled. Harmless for other generators.
#
# This file has no project dependencies on purpose: tests/cmake exercises
# every branch with stand-in executables and throwaway projects.

set(REALMMESH_NINJA_MINIMUM_VERSION "1.11")
set(REALMMESH_NINJA_LINK_POOL "realmmesh_link")

function(realmmesh_check_ninja generator program out_error)
    if(NOT generator MATCHES "^Ninja")
        set(${out_error} "" PARENT_SCOPE)
        return()
    endif()

    string(CONCAT _hint
        "Install Ninja >= ${REALMMESH_NINJA_MINIMUM_VERSION} (macOS: `brew install ninja`; "
        "Ubuntu: `sudo apt-get install ninja-build`) and configure again, or use the "
        "Make fallback explicitly: `cmake --preset dev-make`, or `--preset dev-make` "
        "on the scripts/ entries.")

    if(program STREQUAL "" OR NOT EXISTS "${program}" OR IS_DIRECTORY "${program}")
        set(${out_error}
            "The ${generator} generator needs Ninja, but none was found (CMAKE_MAKE_PROGRAM='${program}'). ${_hint}"
            PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND "${program}" --version
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _status STREQUAL "0")
        set(${out_error}
            "'${program} --version' returned '${_status}' (${_stderr}). ${_hint}" PARENT_SCOPE)
        return()
    endif()
    if(NOT _output MATCHES "^([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
        set(${out_error}
            "'${program} --version' reported '${_output}', which is not a Ninja version. ${_hint}"
            PARENT_SCOPE)
        return()
    endif()
    if(CMAKE_MATCH_1 VERSION_LESS REALMMESH_NINJA_MINIMUM_VERSION)
        set(${out_error}
            "${program} is Ninja ${CMAKE_MATCH_1}; RealmMesh needs ${REALMMESH_NINJA_MINIMUM_VERSION} or newer. ${_hint}"
            PARENT_SCOPE)
        return()
    endif()
    set(${out_error} "" PARENT_SCOPE)
endfunction()

function(realmmesh_require_ninja)
    if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
        return()
    endif()
    if(NOT CMAKE_MAKE_PROGRAM)
        # ninja-build and ninja, as CMake's own lookup tries (it also accepts
        # samu, which is not supported here). The result is cached, so
        # project() then uses exactly the binary checked here.
        find_program(CMAKE_MAKE_PROGRAM NAMES ninja-build ninja
            DOC "Program used to build from build.ninja files.")
    endif()
    realmmesh_check_ninja("${CMAKE_GENERATOR}" "${CMAKE_MAKE_PROGRAM}" _error)
    if(NOT _error STREQUAL "")
        message(FATAL_ERROR "${_error}")
    endif()
endfunction()

function(realmmesh_use_ninja_link_pool)
    if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS "${REALMMESH_NINJA_LINK_POOL}=1")
    set(CMAKE_JOB_POOL_LINK "${REALMMESH_NINJA_LINK_POOL}" PARENT_SCOPE)
endfunction()

function(realmmesh_link_outside_pool target)
    set_property(TARGET "${target}" PROPERTY JOB_POOL_LINK "")
endfunction()
